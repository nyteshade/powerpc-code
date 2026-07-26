#include "agent.hpp"

#include "attach.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace ppcode {

Agent::Agent(Client& client, ToolRegistry& tools, const Config& cfg)
    : client_(client), tools_(tools), cfg_(cfg) {
    char buf[4096];
    cwd_ = getcwd(buf, sizeof(buf)) ? buf : ".";
    ensure_system_prompt();
}

void Agent::ensure_system_prompt() {
    if (!history_.empty() && history_.front().role == "system") return;
    std::string prompt;
    if (!system_override_.empty()) {
        prompt = system_override_;
    } else {
        prompt = cfg_.effective_system_prompt();
        prompt += "\n\nThe working directory is " + cwd_ + ".";
    }
    history_.insert(history_.begin(), Message::system_msg(prompt));
}

void Agent::set_system_prompt(const std::string& text) {
    system_override_ = text;
    if (!history_.empty() && history_.front().role == "system")
        history_.erase(history_.begin());
    ensure_system_prompt();
}

void Agent::reset() {
    history_.clear();
    ensure_system_prompt();
}

void Agent::set_cwd(const std::string& cwd) {
    if (cwd == cwd_) return;
    cwd_ = cwd;
    // Rebuild the system prompt in place so the directory it advertises stays
    // truthful. Dropping and re-inserting keeps it at index 0 without
    // disturbing the rest of the conversation.
    if (!history_.empty() && history_.front().role == "system")
        history_.erase(history_.begin());
    ensure_system_prompt();
}

Agent::RunResult Agent::run(const std::string& user_input, const Events& ev,
                            std::atomic<bool>* cancel) {
    return run(Message::user(user_input), ev, cancel);
}

Agent::RunResult Agent::run(const Message& user_message, const Events& ev,
                            std::atomic<bool>* cancel) {
    ensure_system_prompt();
    history_.push_back(user_message);
    return loop(ev, cancel);
}

Agent::RunResult Agent::run_continuation(const Events& ev, std::atomic<bool>* cancel) {
    ensure_system_prompt();
    return loop(ev, cancel);
}

void Agent::queue_steering(const std::string& text) {
    if (trim(text).empty()) return;
    std::lock_guard<std::mutex> lk(steering_mu_);
    pending_steering_.push_back(text);
}

bool Agent::has_pending_steering() const {
    std::lock_guard<std::mutex> lk(steering_mu_);
    return !pending_steering_.empty();
}

std::vector<std::string> Agent::take_steering() {
    std::lock_guard<std::mutex> lk(steering_mu_);
    std::vector<std::string> out;
    out.swap(pending_steering_);
    return out;
}

Agent::RunResult Agent::loop(const Events& ev, std::atomic<bool>* cancel) {
    RunResult res;
    std::vector<ToolSpec> specs = tools_.specs();

    for (int round = 0; round < cfg_.max_turns; round++) {
        // Steering injected while the previous round was running. This happens
        // at the top of a round -- after the last round's tool results are in
        // the history, and before the next model call -- so the conversation
        // stays well-formed and the model sees the correction immediately.
        for (const std::string& s : take_steering()) {
            history_.push_back(Message::user(s));
            if (ev.on_status) ev.on_status("steering applied");
            log_line("steering: " + elide(s, 120));
        }

        if (cancel && cancel->load()) {
            res.cancelled = true;
            res.error = "cancelled";
            return res;
        }

        res.rounds++;
        if (ev.on_status) ev.on_status(round == 0 ? "thinking" : "thinking (continuing)");

        StreamEvents sev;
        sev.on_text      = ev.on_text;
        sev.on_reasoning = ev.on_reasoning;

        ChatResult cr = client_.chat_stream(history_, specs, sev, cancel);

        res.usage.add(cr.usage);
        session_usage_.add(cr.usage);

        // Spend guardrail. Checked after each round rather than before, because
        // the cost of a round is only known once it has happened -- so the cap
        // is "stop once we have crossed it", not "never exceed it".
        if (cfg_.max_cost > 0.0) {
            double spent = session_usage_.cost;
            if (spent >= cfg_.max_cost) {
                res.hit_cost_limit = true;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "stopping: spent $%.4f, which reaches the $%.4f limit "
                              "for this session (raise it with max_cost or --max-cost)",
                              spent, cfg_.max_cost);
                res.error = buf;
                if (ev.on_error) ev.on_error(res.error);
                return res;
            }
            if (!cost_warned_ && spent >= cfg_.max_cost * cfg_.cost_warn_fraction) {
                cost_warned_ = true;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "spent $%.4f of the $%.4f limit", spent, cfg_.max_cost);
                if (ev.on_status) ev.on_status(buf);
                log_line(buf);
            }
        }

        if (cr.cancelled) {
            res.cancelled = true;
            res.error = "cancelled";
            // Keep whatever partial text arrived so the transcript stays honest.
            if (!cr.message.content.empty()) history_.push_back(cr.message);
            return res;
        }

        if (!cr.ok) {
            res.error = cr.error;
            if (ev.on_error) ev.on_error(cr.error);
            return res;
        }

        history_.push_back(cr.message);
        if (ev.on_assistant_message) ev.on_assistant_message(cr.message);
        if (!cr.message.content.empty()) res.final_text = cr.message.content;

        // No tool calls: the turn is complete -- unless steering arrived while
        // the model was answering, in which case keep going so the correction is
        // not silently dropped.
        if (cr.message.tool_calls.empty()) {
            if (has_pending_steering()) continue;
            res.ok = true;
            return res;
        }

        // Run each requested tool and feed the results back.
        for (const ToolCall& tc : cr.message.tool_calls) {
            if (cancel && cancel->load()) {
                res.cancelled = true;
                res.error = "cancelled";
                // The API requires a tool message for every tool_call id, or the
                // next request is rejected. Fill in the gap.
                history_.push_back(Message::tool_result(tc.id, tc.name,
                                                        "Error: cancelled by user"));
                return res;
            }

            res.tool_calls++;
            if (ev.on_tool_start) ev.on_tool_start(tc);

            std::string parse_err;
            json args = tc.args_json(&parse_err);

            ToolResult tr;
            if (!parse_err.empty() && trim(tc.arguments) != "{}") {
                tr = ToolResult::err(parse_err + " (raw: " +
                                     json_preview(tc.arguments, 200) + ")");
            } else {
                ToolContext ctx;
                ctx.cwd = cwd_;
                ctx.cancel = cancel;
                ctx.note = ev.on_status;
                // yolo bypasses the gate entirely; otherwise the front end's
                // approver decides. With no approver and no yolo, refuse --
                // failing closed is the right default for an unattended run.
                if (cfg_.yolo) {
                    ctx.approve = nullptr;
                } else if (ev.approve) {
                    ctx.approve = ev.approve;
                } else {
                    ctx.approve = [](const std::string& n, ToolKind,
                                     const ToolPreview&) { return false; };
                }
                tr = tools_.call(tc.name, args, ctx);
            }

            if (ev.on_tool_done) ev.on_tool_done(tc, tr);

            // A tool can ask for an image to be shown to the model by prefixing
            // its result with this marker -- the screenshot tool does. The image
            // travels as a separate user message rather than inside the tool
            // result, because tool messages are text-only across most providers.
            std::string content = tr.content;
            std::string image_path;
            if (starts_with(content, kAttachImageMarker)) {
                size_t nl = content.find('\n');
                image_path = trim(content.substr(std::strlen(kAttachImageMarker),
                                                 nl == std::string::npos
                                                     ? std::string::npos
                                                     : nl - std::strlen(kAttachImageMarker)));
                content = (nl == std::string::npos) ? "" : content.substr(nl + 1);
            }

            history_.push_back(Message::tool_result(tc.id, tc.name, content));

            if (!image_path.empty()) {
                std::vector<std::string> warn;
                attach::Loaded l = attach::load(image_path, "image", "auto",
                                                true, cwd_);
                Message m;
                m.role = "user";
                if (l.ok) {
                    m.parts.push_back(ContentPart::make_text(
                        "Here is the image the " + tc.name + " tool produced:"));
                    m.parts.push_back(l.part);
                } else {
                    m.content = "[the " + tc.name + " tool produced an image that "
                                "could not be attached: " + l.error + "]";
                }
                history_.push_back(std::move(m));
            }

            log_line("tool " + tc.name + (tr.is_error ? " -> error" : " -> ok"));
        }
        // Loop back around so the model can see the tool results.
    }

    res.hit_turn_limit = true;
    res.error = "reached the " + std::to_string(cfg_.max_turns) +
                " round limit without finishing";
    if (ev.on_error) ev.on_error(res.error);
    return res;
}

json Agent::to_json() const {
    json j;
    j["version"] = 1;
    j["model"] = cfg_.model;
    j["cwd"] = cwd_;
    json msgs = json::array();
    for (const Message& m : history_) msgs.push_back(m.to_json());
    j["messages"] = msgs;
    j["usage"] = {{"prompt_tokens", session_usage_.prompt_tokens},
                  {"completion_tokens", session_usage_.completion_tokens},
                  {"total_tokens", session_usage_.total_tokens},
                  {"cost", session_usage_.cost}};
    return j;
}

bool Agent::from_json(const json& j, std::string* error) {
    const json* msgs = jptr(j, "messages");
    if (!msgs || !msgs->is_array()) {
        if (error) *error = "session has no messages array";
        return false;
    }
    history_.clear();
    for (const json& m : *msgs) {
        Message msg;
        msg.role = jstr(m, "role");
        if (msg.role.empty()) continue;
        msg.content = jstr(m, "content");
        msg.tool_call_id = jstr(m, "tool_call_id");
        msg.name = jstr(m, "name");
        if (const json* tcs = jptr(m, "tool_calls"); tcs && tcs->is_array()) {
            for (const json& tc : *tcs) {
                ToolCall c;
                c.id = jstr(tc, "id");
                if (const json* fn = jptr(tc, "function")) {
                    c.name = jstr(*fn, "name");
                    c.arguments = jstr(*fn, "arguments", "{}");
                }
                if (!c.name.empty()) msg.tool_calls.push_back(std::move(c));
            }
        }
        history_.push_back(std::move(msg));
    }
    if (std::string c = jstr(j, "cwd"); !c.empty()) cwd_ = c;
    ensure_system_prompt();
    return true;
}

} // namespace ppcode

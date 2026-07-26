#include "agent.hpp"

#include <cstdio>
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
    std::string prompt = cfg_.effective_system_prompt();
    prompt += "\n\nThe working directory is " + cwd_ + ".";
    history_.insert(history_.begin(), Message::system_msg(prompt));
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
    ensure_system_prompt();
    history_.push_back(Message::user(user_input));
    return loop(ev, cancel);
}

Agent::RunResult Agent::run_continuation(const Events& ev, std::atomic<bool>* cancel) {
    ensure_system_prompt();
    return loop(ev, cancel);
}

Agent::RunResult Agent::loop(const Events& ev, std::atomic<bool>* cancel) {
    RunResult res;
    std::vector<ToolSpec> specs = tools_.specs();

    for (int round = 0; round < cfg_.max_turns; round++) {
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

        // No tool calls: the turn is complete.
        if (cr.message.tool_calls.empty()) {
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
            history_.push_back(Message::tool_result(tc.id, tc.name, tr.content));

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

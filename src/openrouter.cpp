#include "openrouter.hpp"
#include "http.hpp"

#include <cstdio>

namespace ppcode {

// ---------------------------------------------------------------------------
// Conversation model
// ---------------------------------------------------------------------------

json ToolCall::args_json(std::string* parse_error) const {
    if (trim(arguments).empty()) return json::object();
    try {
        json j = json::parse(arguments);
        if (!j.is_object()) {
            if (parse_error) *parse_error = "tool arguments were not a JSON object";
            return json::object();
        }
        return j;
    } catch (const std::exception& e) {
        if (parse_error) *parse_error = std::string("malformed tool arguments: ") + e.what();
        return json::object();
    }
}

json Message::to_json() const {
    json m;
    m["role"] = role;

    if (role == "tool") {
        m["content"] = content;
        m["tool_call_id"] = tool_call_id;
        if (!name.empty()) m["name"] = name;
        return m;
    }

    // An assistant turn that only calls tools legitimately has empty content.
    // The API wants the key present regardless.
    m["content"] = content;

    if (!tool_calls.empty()) {
        json arr = json::array();
        for (const ToolCall& tc : tool_calls) {
            json c;
            c["id"] = tc.id;
            c["type"] = "function";
            c["function"] = {{"name", tc.name},
                             {"arguments", tc.arguments.empty() ? "{}" : tc.arguments}};
            arr.push_back(c);
        }
        m["tool_calls"] = arr;
    }
    return m;
}

json ToolSpec::to_json() const {
    json f;
    f["name"] = name;
    f["description"] = description;
    f["parameters"] = parameters.is_null() ? json::object() : parameters;
    return json{{"type", "function"}, {"function", f}};
}

// ---------------------------------------------------------------------------
// StreamAssembler
// ---------------------------------------------------------------------------

void StreamAssembler::apply_delta(const json& delta) {
    // Text content.
    if (const json* c = jptr(delta, "content"); c && c->is_string()) {
        std::string piece = c->get<std::string>();
        if (!piece.empty()) {
            if (!first_token_sent_) {
                first_token_sent_ = true;
                if (ev_.on_first_token) ev_.on_first_token();
            }
            content_ += piece;
            if (ev_.on_text) ev_.on_text(piece);
        }
    }

    // Some models expose a separate reasoning channel.
    if (const json* r = jptr(delta, "reasoning"); r && r->is_string()) {
        std::string piece = r->get<std::string>();
        if (!piece.empty() && ev_.on_reasoning) ev_.on_reasoning(piece);
    }

    // Tool calls arrive in fragments keyed by index: the id and name usually
    // land in the first fragment, then arguments stream in piece by piece.
    if (const json* tcs = jptr(delta, "tool_calls"); tcs && tcs->is_array()) {
        for (const json& tc : *tcs) {
            int idx = static_cast<int>(jint(tc, "index", 0));
            PartialCall& pc = calls_[idx];

            if (std::string id = jstr(tc, "id"); !id.empty()) pc.id = id;

            if (const json* fn = jptr(tc, "function")) {
                if (std::string n = jstr(*fn, "name"); !n.empty()) pc.name = n;
                if (const json* a = jptr(*fn, "arguments"); a && a->is_string())
                    pc.arguments += a->get<std::string>();
            }
        }
    }
}

bool StreamAssembler::feed(const std::string& data) {
    std::string d = trim(data);
    if (d.empty()) return true;
    if (d == "[DONE]") return false;

    json j;
    try {
        j = json::parse(d);
    } catch (const std::exception& e) {
        // A malformed chunk is not worth killing the turn over; note it and
        // keep going.
        log_line(std::string("stream: unparseable chunk: ") + e.what());
        return true;
    }

    // Errors can arrive mid-stream rather than as an HTTP status.
    if (const json* err = jptr(j, "error")) {
        if (err->is_object()) {
            error_ = jstr(*err, "message", err->dump());
            // OpenRouter nests provider errors one level down.
            if (const json* meta = jptr(*err, "metadata")) {
                if (std::string raw = jstr(*meta, "raw"); !raw.empty())
                    error_ += " (" + json_preview(raw, 300) + ")";
            }
        } else {
            error_ = err->dump();
        }
        return false;
    }

    if (std::string m = jstr(j, "model"); !m.empty()) model_ = m;

    if (const json* u = jptr(j, "usage"); u && u->is_object()) {
        usage_.prompt_tokens     = jint(*u, "prompt_tokens");
        usage_.completion_tokens = jint(*u, "completion_tokens");
        usage_.total_tokens      = jint(*u, "total_tokens");
        usage_.cost              = jnum(*u, "cost");
    }

    // The final usage-only chunk carries an empty choices array.
    const json* choices = jptr(j, "choices");
    if (!choices || !choices->is_array() || choices->empty()) return true;

    const json& ch = (*choices)[0];
    if (const json* delta = jptr(ch, "delta"); delta && delta->is_object())
        apply_delta(*delta);

    // Non-streaming payloads sometimes surface here too (e.g. a provider that
    // ignores stream:true). Accept a whole message as one delta.
    if (const json* msg = jptr(ch, "message"); msg && msg->is_object())
        apply_delta(*msg);

    if (std::string fr = jstr(ch, "finish_reason"); !fr.empty()) finish_reason_ = fr;

    return true;
}

Message StreamAssembler::take_message() {
    Message m;
    m.role = "assistant";
    m.content = content_;

    // calls_ is a std::map keyed by index, so iteration is already in the
    // order the model emitted them.
    for (auto& [idx, pc] : calls_) {
        if (pc.name.empty()) continue;      // never got a usable name
        ToolCall tc;
        tc.id = pc.id.empty() ? ("call_" + std::to_string(idx)) : pc.id;
        tc.name = pc.name;
        tc.arguments = pc.arguments.empty() ? "{}" : pc.arguments;
        if (!pc.announced) {
            pc.announced = true;
            if (ev_.on_tool_call) ev_.on_tool_call(tc);
        }
        m.tool_calls.push_back(std::move(tc));
    }
    return m;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

http::Headers Client::auth_headers() const {
    http::Headers h;
    h.push_back("Authorization: Bearer " + cfg_.api_key);
    if (!cfg_.referer.empty()) h.push_back("HTTP-Referer: " + cfg_.referer);
    if (!cfg_.title.empty())   h.push_back("X-Title: " + cfg_.title);
    return h;
}

json Client::build_request(const std::vector<Message>& messages,
                           const std::vector<ToolSpec>& tools, bool stream) const {
    json req;
    req["model"] = cfg_.model;

    json msgs = json::array();
    for (const Message& m : messages) msgs.push_back(m.to_json());
    req["messages"] = msgs;

    req["stream"] = stream;
    if (cfg_.max_tokens > 0)   req["max_tokens"] = cfg_.max_tokens;
    if (cfg_.temperature >= 0) req["temperature"] = cfg_.temperature;

    if (!tools.empty()) {
        json arr = json::array();
        for (const ToolSpec& t : tools) arr.push_back(t.to_json());
        req["tools"] = arr;
        req["tool_choice"] = "auto";
    }

    if (stream) {
        // Ask for a usage block on the final chunk.
        req["stream_options"] = {{"include_usage", true}};
    }
    return req;
}

// Pull a useful message out of an OpenRouter/provider error body.
static std::string extract_error(const std::string& body, long status) {
    std::string fallback = "HTTP " + std::to_string(status);
    if (trim(body).empty()) return fallback;
    try {
        json j = json::parse(body);
        if (const json* e = jptr(j, "error")) {
            if (e->is_object()) {
                std::string msg = jstr(*e, "message");
                if (!msg.empty()) return fallback + ": " + msg;
            } else if (e->is_string()) {
                return fallback + ": " + e->get<std::string>();
            }
        }
        if (std::string m = jstr(j, "message"); !m.empty()) return fallback + ": " + m;
    } catch (...) {
        // fall through to the raw body
    }
    return fallback + ": " + json_preview(body, 300);
}

ChatResult Client::chat_stream(const std::vector<Message>& messages,
                               const std::vector<ToolSpec>& tools,
                               const StreamEvents& ev,
                               std::atomic<bool>* cancel) {
    ChatResult out;

    if (cfg_.api_key.empty()) {
        out.error = "no API key: set OPENROUTER_AI_API_KEY";
        return out;
    }

    std::string body = build_request(messages, tools, true).dump();
    log_line("POST /chat/completions model=" + cfg_.model +
             " messages=" + std::to_string(messages.size()) +
             " tools=" + std::to_string(tools.size()));

    StreamAssembler asm_(ev);
    auto handler = [&](const http::SseEvent& e) -> bool {
        return asm_.feed(e.data);
    };

    http::Response r = http::post_sse(cfg_.base_url + "/chat/completions",
                                      auth_headers(), body, handler, cancel);

    out.cancelled = r.cancelled;
    out.message = asm_.take_message();
    out.usage = asm_.usage();
    out.finish_reason = asm_.finish_reason();
    out.model = asm_.model().empty() ? cfg_.model : asm_.model();

    if (r.cancelled) {
        out.error = "cancelled";
        return out;
    }
    if (!r.error.empty()) {
        out.error = "network: " + r.error;
        return out;
    }
    if (r.status < 200 || r.status >= 300) {
        out.error = extract_error(r.body, r.status);
        return out;
    }
    if (asm_.had_error()) {
        out.error = asm_.error();
        return out;
    }

    // A turn with neither text nor tool calls means something went wrong
    // upstream, even though the transport succeeded.
    if (out.message.content.empty() && out.message.tool_calls.empty()) {
        out.error = "empty response from model";
        return out;
    }

    out.ok = true;
    return out;
}

ChatResult Client::chat(const std::vector<Message>& messages,
                        const std::vector<ToolSpec>& tools,
                        std::atomic<bool>* cancel) {
    ChatResult out;
    if (cfg_.api_key.empty()) {
        out.error = "no API key: set OPENROUTER_AI_API_KEY";
        return out;
    }

    std::string body = build_request(messages, tools, false).dump();
    http::Response r = http::post_json(cfg_.base_url + "/chat/completions",
                                       auth_headers(), body, 600);

    if (!r.error.empty()) { out.error = "network: " + r.error; return out; }
    if (r.status < 200 || r.status >= 300) {
        out.error = extract_error(r.body, r.status);
        return out;
    }

    try {
        json j = json::parse(r.body);
        if (const json* e = jptr(j, "error")) {
            out.error = e->is_object() ? jstr(*e, "message", e->dump()) : e->dump();
            return out;
        }

        out.model = jstr(j, "model", cfg_.model);
        if (const json* u = jptr(j, "usage")) {
            out.usage.prompt_tokens     = jint(*u, "prompt_tokens");
            out.usage.completion_tokens = jint(*u, "completion_tokens");
            out.usage.total_tokens      = jint(*u, "total_tokens");
            out.usage.cost              = jnum(*u, "cost");
        }

        const json* choices = jptr(j, "choices");
        if (!choices || !choices->is_array() || choices->empty()) {
            out.error = "response contained no choices";
            return out;
        }
        const json& ch = (*choices)[0];
        out.finish_reason = jstr(ch, "finish_reason");

        const json* msg = jptr(ch, "message");
        if (!msg) { out.error = "response choice had no message"; return out; }

        out.message.role = "assistant";
        out.message.content = jstr(*msg, "content");

        if (const json* tcs = jptr(*msg, "tool_calls"); tcs && tcs->is_array()) {
            for (const json& tc : *tcs) {
                ToolCall c;
                c.id = jstr(tc, "id");
                if (const json* fn = jptr(tc, "function")) {
                    c.name = jstr(*fn, "name");
                    c.arguments = jstr(*fn, "arguments", "{}");
                }
                if (!c.name.empty()) out.message.tool_calls.push_back(std::move(c));
            }
        }
        out.ok = true;
        return out;
    } catch (const std::exception& e) {
        out.error = std::string("could not parse response: ") + e.what();
        return out;
    }
}

std::vector<ModelInfo> Client::list_models(std::string* error) {
    std::vector<ModelInfo> out;
    http::Response r = http::get(cfg_.base_url + "/models", auth_headers(), 60);
    if (!r.error.empty()) {
        if (error) *error = r.error;
        return out;
    }
    if (r.status != 200) {
        if (error) *error = extract_error(r.body, r.status);
        return out;
    }
    try {
        json j = json::parse(r.body);
        const json* data = jptr(j, "data");
        if (!data || !data->is_array()) {
            if (error) *error = "unexpected /models payload";
            return out;
        }
        for (const json& m : *data) {
            ModelInfo mi;
            mi.id = jstr(m, "id");
            if (mi.id.empty()) continue;
            mi.name = jstr(m, "name", mi.id);
            mi.context_length = jint(m, "context_length");
            if (const json* p = jptr(m, "pricing")) {
                // Pricing arrives as decimal strings.
                mi.prompt_cost     = std::atof(jstr(*p, "prompt", "0").c_str());
                mi.completion_cost = std::atof(jstr(*p, "completion", "0").c_str());
            }
            if (const json* sp = jptr(m, "supported_parameters"); sp && sp->is_array()) {
                for (const json& s : *sp)
                    if (s.is_string() && s.get<std::string>() == "tools")
                        mi.supports_tools = true;
            }
            out.push_back(std::move(mi));
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return out;
}

bool Client::get_credits(double* remaining, std::string* error) {
    http::Response r = http::get(cfg_.base_url + "/credits", auth_headers(), 30);
    if (!r.error.empty() || r.status != 200) {
        if (error) *error = r.error.empty() ? extract_error(r.body, r.status) : r.error;
        return false;
    }
    try {
        json j = json::parse(r.body);
        const json* d = jptr(j, "data");
        if (!d) return false;
        double total = jnum(*d, "total_credits");
        double used  = jnum(*d, "total_usage");
        if (remaining) *remaining = total - used;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace ppcode

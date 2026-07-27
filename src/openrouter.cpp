#include "openrouter.hpp"
#include "http.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include <unistd.h>   // usleep, for retry backoff

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

json ContentPart::to_json() const {
    switch (type) {
        case Type::ImageUrl: {
            json img = {{"url", url}};
            if (!detail.empty() && detail != "auto") img["detail"] = detail;
            return json{{"type", "image_url"}, {"image_url", img}};
        }
        case Type::FileData: {
            json f = {{"file_data", url}};
            if (!filename.empty()) f["filename"] = filename;
            return json{{"type", "file"}, {"file", f}};
        }
        case Type::Text:
        default:
            return json{{"type", "text"}, {"text", text}};
    }
}

std::string Message::display_text() const {
    if (parts.empty()) return content;
    std::string out;
    for (const ContentPart& p : parts) {
        if (p.type == ContentPart::Type::Text) {
            if (!out.empty()) out += "\n";
            out += p.text;
        } else if (p.type == ContentPart::Type::ImageUrl) {
            if (!out.empty()) out += "\n";
            // A data: URI is megabytes of base64; never show it.
            out += starts_with(p.url, "data:")
                       ? "[image: " + (p.mime.empty() ? "embedded" : p.mime) + "]"
                       : "[image: " + p.url + "]";
        } else {
            if (!out.empty()) out += "\n";
            out += "[file: " + (p.filename.empty() ? "attached" : p.filename) + "]";
        }
    }
    return out;
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

    if (!parts.empty()) {
        json arr = json::array();
        for (const ContentPart& p : parts) arr.push_back(p.to_json());
        m["content"] = arr;
        if (!tool_calls.empty()) {
            json tarr = json::array();
            for (const ToolCall& tc : tool_calls) {
                json c;
                c["id"] = tc.id;
                c["type"] = "function";
                c["function"] = {{"name", tc.name},
                                 {"arguments", tc.arguments.empty() ? "{}" : tc.arguments}};
                tarr.push_back(c);
            }
            m["tool_calls"] = tarr;
        }
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
        // Providers report cache hits in slightly different shapes.
        if (const json* d = jptr(*u, "prompt_tokens_details"); d && d->is_object()) {
            usage_.cached_tokens = jint(*d, "cached_tokens");
            usage_.cache_write_tokens = jint(*d, "cache_creation_tokens");
        }
        if (usage_.cached_tokens == 0)
            usage_.cached_tokens = jint(*u, "cached_tokens");
        // DeepSeek's spelling.
        if (usage_.cached_tokens == 0)
            usage_.cached_tokens = jint(*u, "prompt_cache_hit_tokens");
        if (usage_.cache_write_tokens == 0)
            usage_.cache_write_tokens = jint(*u, "cache_creation_input_tokens");
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
    // Attribution headers are an OpenRouter convention. Sending them to
    // another service is at best ignored and at worst rejected.
    const Provider* prov = cfg_.provider_info;
    if (!prov || prov->attribution_headers) {
        if (!cfg_.referer.empty()) h.push_back("HTTP-Referer: " + cfg_.referer);
        if (!cfg_.title.empty())   h.push_back("X-Title: " + cfg_.title);
    }
    return h;
}

// Which providers need an explicit cache breakpoint. OpenAI caches long prompts
// automatically and rejects nothing if we stay quiet; Anthropic and Google only
// cache what is marked.
static bool wants_cache_control(const std::string& model, const std::string& mode) {
    if (mode == "off") return false;
    if (mode == "on") return true;
    std::string m = to_lower(model);
    return starts_with(m, "anthropic/") || starts_with(m, "google/") ||
           m.find("claude") != std::string::npos ||
           m.find("gemini") != std::string::npos;
}

// Rewrite a message's content into the array form with a cache breakpoint on the
// final block. Everything before the breakpoint is cached by the provider.
static void mark_cached(json* msg) {
    if (!msg->is_object()) return;
    auto it = msg->find("content");
    if (it == msg->end()) return;

    json blocks;
    if (it->is_string()) {
        std::string text = it->get<std::string>();
        if (trim(text).empty()) return;
        blocks = json::array({json{{"type", "text"}, {"text", text}}});
    } else if (it->is_array()) {
        if (it->empty()) return;
        blocks = *it;
    } else {
        return;
    }
    // Attach to the last text block; a breakpoint on an image is meaningless.
    for (auto b = blocks.rbegin(); b != blocks.rend(); ++b) {
        if (b->is_object() && (*b)["type"] == "text") {
            (*b)["cache_control"] = json{{"type", "ephemeral"}};
            break;
        }
    }
    (*msg)["content"] = blocks;
}

json Client::build_request(const std::vector<Message>& messages,
                           const std::vector<ToolSpec>& tools, bool stream) const {
    json req;
    req["model"] = cfg_.model;

    json msgs = json::array();
    for (const Message& m : messages) msgs.push_back(m.to_json());

    // The system message is the same thousands of tokens on every round of a
    // turn -- machine probe, platform knowledge, tool guidance. Caching it is
    // the difference between paying for it once and paying for it every round.
    if (wants_cache_control(cfg_.model, cfg_.cache_mode) && !msgs.empty()) {
        if (msgs[0].is_object() && msgs[0]["role"] == "system") mark_cached(&msgs[0]);

        // A second breakpoint just before the newest turn lets the accumulated
        // conversation be reused as tool results pile up. Anthropic allows four;
        // two is enough and keeps the request simple.
        if (msgs.size() >= 3) {
            size_t idx = msgs.size() - 2;
            if (msgs[idx].is_object()) mark_cached(&msgs[idx]);
        }
    }
    req["messages"] = msgs;

    if (!cfg_.model_fallbacks.empty()) {
        // OpenRouter tries these in order if the primary model is unavailable.
        json alts = json::array();
        alts.push_back(cfg_.model);
        for (const std::string& m : cfg_.model_fallbacks) alts.push_back(m);
        req["models"] = alts;
    }

    req["stream"] = stream;
    if (cfg_.max_tokens > 0)   req["max_tokens"] = cfg_.max_tokens;
    if (cfg_.temperature >= 0) req["temperature"] = cfg_.temperature;
    if (cfg_.top_p > 0)        req["top_p"] = cfg_.top_p;
    if (cfg_.seed >= 0)        req["seed"] = cfg_.seed;

    const Provider* prov = cfg_.provider_info;
    bool routing_ok = !prov || prov->supports_routing;
    bool plugins_ok = !prov || prov->supports_plugins;

    if (routing_ok && cfg_.provider.is_object() && !cfg_.provider.empty())
        req["provider"] = cfg_.provider;
    if (cfg_.reasoning.is_object() && !cfg_.reasoning.empty())
        req["reasoning"] = cfg_.reasoning;

    if (cfg_.web_search && plugins_ok) {
        json plugin = {{"id", "web"}};
        if (cfg_.web_max_results > 0) plugin["max_results"] = cfg_.web_max_results;
        req["plugins"] = json::array({plugin});
    }

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

// Worth another attempt? Rate limits and gateway errors are transient; a 400 or
// a 401 will fail identically every time and retrying just wastes the user's
// time and money.
static bool retryable_status(long status) {
    return status == 408 || status == 409 || status == 429 ||
           (status >= 500 && status < 600);
}

static bool retryable_transport(const std::string& err) {
    if (err.empty()) return false;
    std::string e = to_lower(err);
    return e.find("timed out") != std::string::npos ||
           e.find("timeout") != std::string::npos ||
           e.find("connection") != std::string::npos ||
           e.find("resolve") != std::string::npos ||
           e.find("recv failure") != std::string::npos ||
           e.find("send failure") != std::string::npos ||
           e.find("ssl") != std::string::npos;
}

// Exponential backoff with jitter. The jitter matters because an agentic loop
// retrying in lockstep with itself just rebuilds the burst that caused the 429.
static int backoff_ms(int attempt) {
    int base = 800 << (attempt < 5 ? attempt : 5);      // 0.8s, 1.6s, 3.2s, ...
    if (base > 30000) base = 30000;
    // Cheap deterministic jitter; no need for real randomness here.
    static unsigned seed = 0x9E3779B9u;
    seed = seed * 1664525u + 1013904223u;
    int jitter = static_cast<int>(seed % 400);
    return base + jitter;
}

static void sleep_ms(int ms, std::atomic<bool>* cancel) {
    // Wake regularly so Ctrl+C during a backoff is still responsive.
    for (int slept = 0; slept < ms; slept += 100) {
        if (cancel && cancel->load()) return;
        usleep(100 * 1000);
    }
}


namespace {

// DeepSeek and a local server report token counts but no money. Filling the
// figure in from the provider's price table is what keeps --max-cost, the
// status line and the session totals meaningful rather than stuck at zero --
// and a zero there would read as "free", not as "unknown".
void fill_missing_cost(const Config& cfg, Usage* u) {
    if (!u || u->cost > 0.0) return;

    const Provider* p = cfg.provider_info;
    if (!p || p->cost_in_usage) return;

    u->cost = estimate_cost(*p, cfg.model, u->prompt_tokens,
                            u->completion_tokens, u->cached_tokens);
}

} // namespace

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

    const int attempts = cfg_.max_retries > 0 ? cfg_.max_retries + 1 : 1;

    for (int attempt = 0; attempt < attempts; attempt++) {
        if (cancel && cancel->load()) {
            out.cancelled = true;
            out.error = "cancelled";
            return out;
        }

        // Whether anything reached the user this attempt. Once a single token
        // has been emitted we can never retry: the caller has already shown it,
        // and a second attempt would duplicate the text.
        bool emitted = false;
        StreamEvents guarded = ev;
        guarded.on_text = [&](const std::string& d) {
            emitted = true;
            if (ev.on_text) ev.on_text(d);
        };
        guarded.on_reasoning = [&](const std::string& d) {
            emitted = true;
            if (ev.on_reasoning) ev.on_reasoning(d);
        };

        StreamAssembler asm_(guarded);
        auto handler = [&](const http::SseEvent& e) -> bool {
            return asm_.feed(e.data);
        };

        http::Response r = http::post_sse(cfg_.base_url + "/chat/completions",
                                          auth_headers(), body, handler, cancel);

        ChatResult attempt_result;
        attempt_result.cancelled = r.cancelled;
        attempt_result.message = asm_.take_message();
        attempt_result.usage = asm_.usage();
        fill_missing_cost(cfg_, &attempt_result.usage);
        attempt_result.finish_reason = asm_.finish_reason();
        attempt_result.model = asm_.model().empty() ? cfg_.model : asm_.model();

        bool transient = false;
        if (r.cancelled) {
            attempt_result.error = "cancelled";
        } else if (!r.error.empty()) {
            attempt_result.error = "network: " + r.error;
            transient = retryable_transport(r.error);
        } else if (r.status < 200 || r.status >= 300) {
            attempt_result.error = extract_error(r.body, r.status);
            transient = retryable_status(r.status);
        } else if (asm_.had_error()) {
            attempt_result.error = asm_.error();
            // Mid-stream provider errors are frequently rate limits.
            std::string e = to_lower(attempt_result.error);
            transient = e.find("rate") != std::string::npos ||
                        e.find("overloaded") != std::string::npos ||
                        e.find("try again") != std::string::npos ||
                        e.find("capacity") != std::string::npos;
        } else if (attempt_result.message.content.empty() &&
                   attempt_result.message.tool_calls.empty()) {
            attempt_result.error = "empty response from model";
            transient = true;   // usually an upstream hiccup
        } else {
            attempt_result.ok = true;
        }

        if (attempt_result.ok || attempt_result.cancelled) return attempt_result;

        bool can_retry = transient && !emitted && (attempt + 1 < attempts);
        if (!can_retry) {
            if (emitted && transient)
                attempt_result.error +=
                    " (not retried: part of the reply had already been shown)";
            return attempt_result;
        }

        int wait = backoff_ms(attempt);
        log_line("retrying after " + std::to_string(wait) + "ms: " +
                 attempt_result.error);
        if (ev.on_reasoning) {
            // Surface the wait so the UI does not look hung.
        }
        sleep_ms(wait, cancel);
        out = attempt_result;   // keep the last error if we run out of attempts
    }
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
            if (const json* d = jptr(*u, "prompt_tokens_details"); d && d->is_object())
                out.usage.cached_tokens = jint(*d, "cached_tokens");
            if (out.usage.cached_tokens == 0)
                out.usage.cached_tokens = jint(*u, "prompt_cache_hit_tokens");
        }
        fill_missing_cost(cfg_, &out.usage);

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
        const Provider* prov = cfg_.provider_info;

        for (const json& m : *data) {
            ModelInfo mi;
            mi.id = jstr(m, "id");
            if (mi.id.empty()) continue;

            // A listing that is only ids -- DeepSeek returns exactly id,
            // object and owned_by -- carries no context window and no prices,
            // so the local table supplies them. Without this the context
            // budget and the spend cap would both be guessing.
            if (prov && !prov->models_have_metadata) {
                if (const ModelDefaults* d = prov->model_defaults(mi.id)) {
                    mi.name = d->id;
                    mi.context_length = d->context_length;
                    mi.max_completion_tokens = d->max_completion_tokens;
                    mi.prompt_cost = d->prompt_cost;
                    mi.completion_cost = d->completion_cost;
                    mi.supports_tools = d->supports_tools;
                    mi.supports_reasoning = d->supports_reasoning;
                    mi.supports_images = d->supports_images;
                    mi.description = d->description;
                    out.push_back(mi);
                    continue;
                }

                // An id we have no table entry for: keep it selectable, but
                // say nothing about it rather than inventing a context window.
                mi.name = mi.id;
                mi.supports_tools = true;
                out.push_back(mi);
                continue;
            }
            mi.name = jstr(m, "name", mi.id);
            mi.context_length = jint(m, "context_length");
            if (const json* p = jptr(m, "pricing")) {
                // Pricing arrives as decimal strings.
                mi.prompt_cost     = std::atof(jstr(*p, "prompt", "0").c_str());
                mi.completion_cost = std::atof(jstr(*p, "completion", "0").c_str());
            }
            mi.description = jstr(m, "description");
            if (const json* tp = jptr(m, "top_provider"))
                mi.max_completion_tokens = jint(*tp, "max_completion_tokens");

            if (const json* sp = jptr(m, "supported_parameters"); sp && sp->is_array()) {
                for (const json& s : *sp) {
                    if (!s.is_string()) continue;
                    std::string param = s.get<std::string>();
                    if (param == "tools" || param == "tool_choice")
                        mi.supports_tools = true;
                    else if (param == "reasoning" || param == "include_reasoning")
                        mi.supports_reasoning = true;
                }
            }
            // Input modalities live under architecture; older payloads used a
            // bare "modality" string like "text+image->text".
            if (const json* arch = jptr(m, "architecture")) {
                if (const json* im = jptr(*arch, "input_modalities");
                    im && im->is_array()) {
                    for (const json& s : *im) {
                        if (!s.is_string()) continue;
                        std::string mod = s.get<std::string>();
                        mi.input_modalities.push_back(mod);
                        if (mod == "image") mi.supports_images = true;
                        if (mod == "audio") mi.supports_audio = true;
                    }
                }
                if (mi.input_modalities.empty()) {
                    std::string modality = jstr(*arch, "modality");
                    if (modality.find("image") != std::string::npos) {
                        mi.supports_images = true;
                        mi.input_modalities = {"text", "image"};
                    } else if (!modality.empty()) {
                        mi.input_modalities = {"text"};
                    }
                }
            }
            out.push_back(std::move(mi));
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return out;
}

// ---------------------------------------------------------------------------
// ModelCatalog
// ---------------------------------------------------------------------------

const std::vector<std::string>& favorite_models() {
    // Ordered by ascending capability and cost, per the user's stated
    // preference, with the Anthropic default first since it is the built-in.
    static const std::vector<std::string> favs = {
        "anthropic/claude-sonnet-5",
        "deepseek/deepseek-v4-pro",
        "z-ai/glm-5.2",
        "moonshotai/kimi-k3",
    };
    return favs;
}

std::string ModelCatalog::cache_path() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::string(xdg) + "/ppcode/models.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.cache/ppcode/models.json";
    return "/tmp/ppcode-models.json";
}

void ModelCatalog::reindex() {
    by_id_.clear();
    for (size_t i = 0; i < models_.size(); i++) by_id_[models_[i].id] = i;
}

bool ModelCatalog::read_cache(int64_t max_age_s) {
    std::string text;
    if (!read_file_text(cache_path(), &text, nullptr)) return false;
    try {
        json j = json::parse(text);
        int64_t stamp = jint(j, "fetched_at");
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (max_age_s > 0 && (now - stamp) > max_age_s) return false;

        const json* arr = jptr(j, "models");
        if (!arr || !arr->is_array()) return false;

        models_.clear();
        for (const json& m : *arr) {
            ModelInfo mi;
            mi.id = jstr(m, "id");
            if (mi.id.empty()) continue;
            mi.name = jstr(m, "name", mi.id);
            mi.context_length = jint(m, "context_length");
            mi.max_completion_tokens = jint(m, "max_completion_tokens");
            mi.prompt_cost = jnum(m, "prompt_cost");
            mi.completion_cost = jnum(m, "completion_cost");
            mi.supports_tools = jbool(m, "supports_tools");
            mi.supports_reasoning = jbool(m, "supports_reasoning");
            mi.supports_images = jbool(m, "supports_images");
            mi.supports_audio = jbool(m, "supports_audio");
            if (const json* im = jptr(m, "input_modalities"); im && im->is_array())
                for (const json& s : *im)
                    if (s.is_string()) mi.input_modalities.push_back(s.get<std::string>());
            models_.push_back(std::move(mi));
        }
        reindex();
        return !models_.empty();
    } catch (const std::exception&) {
        return false;
    }
}

void ModelCatalog::write_cache() const {
    json arr = json::array();
    for (const ModelInfo& m : models_) {
        arr.push_back({{"id", m.id},
                       {"name", m.name},
                       {"context_length", m.context_length},
                       {"max_completion_tokens", m.max_completion_tokens},
                       {"prompt_cost", m.prompt_cost},
                       {"completion_cost", m.completion_cost},
                       {"supports_tools", m.supports_tools},
                       {"supports_reasoning", m.supports_reasoning},
                       {"supports_images", m.supports_images},
                       {"supports_audio", m.supports_audio},
                       {"input_modalities", m.input_modalities}});
    }
    json j;
    j["fetched_at"] = static_cast<int64_t>(std::time(nullptr));
    j["models"] = arr;

    std::string path = cache_path();
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::string err;
    if (!write_file_text(path, j.dump(), &err))
        log_line("model catalog: could not cache: " + err);
}

bool ModelCatalog::load(Client& client, std::string* error, bool force_refresh,
                        int64_t max_age_s) {
    if (!force_refresh && read_cache(max_age_s)) return true;

    std::string err;
    std::vector<ModelInfo> fetched = client.list_models(&err);
    if (fetched.empty()) {
        // A stale cache beats nothing at all when the network is down.
        if (read_cache(0)) {
            log_line("model catalog: fetch failed, using stale cache: " + err);
            return true;
        }
        if (error) *error = err.empty() ? "no models returned" : err;
        return false;
    }
    models_ = std::move(fetched);
    reindex();
    write_cache();
    return true;
}

const ModelInfo* ModelCatalog::find(const std::string& id) const {
    auto it = by_id_.find(id);
    if (it != by_id_.end()) return &models_[it->second];

    // OpenRouter accepts suffixes like ":online", ":free" and ":nitro" that are
    // routing hints rather than distinct models; fall back to the base id.
    size_t colon = id.find(':');
    if (colon != std::string::npos) {
        auto base = by_id_.find(id.substr(0, colon));
        if (base != by_id_.end()) return &models_[base->second];
    }
    return nullptr;
}

int64_t ModelCatalog::context_for(const std::string& id) const {
    const ModelInfo* m = find(id);
    if (m && m->context_length > 0) return m->context_length;
    return kUnknownContext;
}

std::vector<const ModelInfo*> ModelCatalog::search(const std::string& query,
                                                   size_t limit) const {
    std::string q = to_lower(trim(query));
    std::vector<std::pair<int, const ModelInfo*>> scored;

    for (const ModelInfo& m : models_) {
        if (q.empty()) {
            scored.emplace_back(0, &m);
            continue;
        }
        std::string lid = to_lower(m.id);
        std::string lname = to_lower(m.name);

        int score;
        if (lid == q)                                     score = 0;
        else if (starts_with(lid, q))                     score = 1;
        else if (lid.find('/') != std::string::npos &&
                 starts_with(lid.substr(lid.find('/') + 1), q)) score = 2;
        else if (lid.find(q) != std::string::npos)        score = 3;
        else if (lname.find(q) != std::string::npos)      score = 4;
        else {
            // Match on all whitespace-separated terms in any order.
            bool all = true;
            for (const std::string& term : split(q, ' ')) {
                if (term.empty()) continue;
                if (lid.find(term) == std::string::npos &&
                    lname.find(term) == std::string::npos) { all = false; break; }
            }
            if (!all) continue;
            score = 5;
        }
        scored.emplace_back(score, &m);
    }

    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<const ModelInfo*> out;
    for (const auto& [s, m] : scored) {
        out.push_back(m);
        if (out.size() >= limit) break;
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

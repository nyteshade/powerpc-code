// openrouter.hpp -- OpenRouter chat completions, streaming, and tool calling.
#pragma once

#include "common.hpp"
#include "config.hpp"
#include "http.hpp"

#include <atomic>

namespace ppcode {

// ---------------------------------------------------------------------------
// Conversation model
// ---------------------------------------------------------------------------

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;   // raw JSON text as the model emitted it

    // Parse `arguments`, returning an empty object if it is malformed. Models
    // do occasionally emit truncated JSON, and a tool erroring out is a better
    // outcome than the whole turn dying.
    json args_json(std::string* parse_error = nullptr) const;
};

struct Message {
    std::string role;                  // "system" | "user" | "assistant" | "tool"
    std::string content;
    std::vector<ToolCall> tool_calls;  // assistant turns that call tools
    std::string tool_call_id;          // required on "tool" turns
    std::string name;                  // tool name, for readability

    json to_json() const;

    static Message system_msg(const std::string& c)    { return {"system", c, {}, "", ""}; }
    static Message user(const std::string& c)          { return {"user", c, {}, "", ""}; }
    static Message assistant(const std::string& c)     { return {"assistant", c, {}, "", ""}; }
    static Message tool_result(const std::string& call_id, const std::string& tool_name,
                               const std::string& content) {
        return {"tool", content, {}, call_id, tool_name};
    }
};

// A tool as advertised to the model.
struct ToolSpec {
    std::string name;
    std::string description;
    json parameters;         // JSON Schema object
    json to_json() const;
};

struct Usage {
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    int64_t total_tokens = 0;
    double cost = 0.0;
    void add(const Usage& o) {
        prompt_tokens += o.prompt_tokens;
        completion_tokens += o.completion_tokens;
        total_tokens += o.total_tokens;
        cost += o.cost;
    }
};

// Callbacks fired as a streamed response arrives. All are optional.
struct StreamEvents {
    std::function<void(const std::string&)> on_text;       // assistant content delta
    std::function<void(const std::string&)> on_reasoning;  // reasoning delta, if the model emits it
    std::function<void(const ToolCall&)> on_tool_call;     // fired once per call, when complete
    std::function<void()> on_first_token;
};

struct ChatResult {
    bool ok = false;
    std::string error;          // human-readable failure
    Message message;            // assembled assistant message
    Usage usage;
    std::string finish_reason;  // "stop" | "tool_calls" | "length" | ...
    bool cancelled = false;
    std::string model;          // model that actually served the request
};

struct ModelInfo {
    std::string id;
    std::string name;
    int64_t context_length = 0;
    double prompt_cost = 0.0;      // USD per token
    double completion_cost = 0.0;
    bool supports_tools = false;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

class Client {
public:
    explicit Client(const Config& cfg) : cfg_(cfg) {}

    // Streaming chat. Blocks until the response completes, invoking `ev` as
    // deltas arrive. Set `*cancel` to abort mid-stream.
    ChatResult chat_stream(const std::vector<Message>& messages,
                           const std::vector<ToolSpec>& tools,
                           const StreamEvents& ev,
                           std::atomic<bool>* cancel = nullptr);

    // Non-streaming, for scripted use where incremental output is pointless.
    ChatResult chat(const std::vector<Message>& messages,
                    const std::vector<ToolSpec>& tools,
                    std::atomic<bool>* cancel = nullptr);

    std::vector<ModelInfo> list_models(std::string* error);

    // Remaining credit, for the status line. Returns false if unavailable.
    bool get_credits(double* remaining, std::string* error);

    const Config& config() const { return cfg_; }
    void set_model(const std::string& m) { cfg_.model = m; }

private:
    Config cfg_;

    http::Headers auth_headers() const;
    json build_request(const std::vector<Message>& messages,
                       const std::vector<ToolSpec>& tools, bool stream) const;
};

// ---------------------------------------------------------------------------
// StreamAssembler -- turns a sequence of SSE `data:` payloads into a finished
// assistant Message. Split out from Client so it can be tested without a
// network, since incremental tool-call assembly is the fiddliest part of the
// whole protocol.
// ---------------------------------------------------------------------------

class StreamAssembler {
public:
    explicit StreamAssembler(const StreamEvents& ev) : ev_(ev) {}

    // Feed one SSE data payload. Returns false when the stream is finished
    // (i.e. "[DONE]" was seen).
    bool feed(const std::string& data);

    Message take_message();
    const Usage& usage() const { return usage_; }
    const std::string& finish_reason() const { return finish_reason_; }
    const std::string& model() const { return model_; }
    const std::string& error() const { return error_; }
    bool had_error() const { return !error_.empty(); }

private:
    struct PartialCall {
        std::string id;
        std::string name;
        std::string arguments;
        bool announced = false;
    };

    const StreamEvents& ev_;
    std::string content_;
    std::map<int, PartialCall> calls_;   // keyed by the delta's "index"
    Usage usage_;
    std::string finish_reason_;
    std::string model_;
    std::string error_;
    bool first_token_sent_ = false;

    void apply_delta(const json& delta);
};

} // namespace ppcode

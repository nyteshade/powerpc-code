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

// A piece of a multi-part message. Text-only messages do not need these -- they
// exist so that images and file contents can travel alongside a prompt for
// models that accept them.
struct ContentPart {
    enum class Type { Text, ImageUrl, FileData };

    Type type = Type::Text;
    std::string text;        // Type::Text
    std::string url;         // Type::ImageUrl -- http(s):// or a data: URI
    std::string detail;      // Type::ImageUrl -- "auto" | "low" | "high"
    std::string filename;    // Type::FileData
    std::string mime;

    json to_json() const;

    static ContentPart make_text(const std::string& t) {
        ContentPart p;
        p.type = Type::Text;
        p.text = t;
        return p;
    }
    static ContentPart make_image(const std::string& url,
                                  const std::string& detail = "auto") {
        ContentPart p;
        p.type = Type::ImageUrl;
        p.url = url;
        p.detail = detail;
        return p;
    }
};

struct Message {
    std::string role;                  // "system" | "user" | "assistant" | "tool"
    std::string content;
    std::vector<ContentPart> parts;    // when non-empty, sent instead of content
    std::vector<ToolCall> tool_calls;  // assistant turns that call tools
    std::string tool_call_id;          // required on "tool" turns
    std::string name;                  // tool name, for readability

    json to_json() const;

    // Text of the message regardless of how it is carried, for display and for
    // saving sessions.
    std::string display_text() const;

    static Message system_msg(const std::string& c) {
        Message m; m.role = "system"; m.content = c; return m;
    }
    static Message user(const std::string& c) {
        Message m; m.role = "user"; m.content = c; return m;
    }
    static Message assistant(const std::string& c) {
        Message m; m.role = "assistant"; m.content = c; return m;
    }
    static Message tool_result(const std::string& call_id, const std::string& tool_name,
                               const std::string& content) {
        Message m;
        m.role = "tool";
        m.content = content;
        m.tool_call_id = call_id;
        m.name = tool_name;
        return m;
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

    // Prompt-cache accounting, so the saving from caching the system message is
    // observable rather than assumed.
    int64_t cached_tokens = 0;      // prompt tokens served from cache
    int64_t cache_write_tokens = 0; // tokens written into the cache

    void add(const Usage& o) {
        prompt_tokens += o.prompt_tokens;
        completion_tokens += o.completion_tokens;
        total_tokens += o.total_tokens;
        cost += o.cost;
        cached_tokens += o.cached_tokens;
        cache_write_tokens += o.cache_write_tokens;
    }
    // Fraction of prompt tokens that came from cache.
    double cache_hit_rate() const {
        return prompt_tokens > 0
                   ? static_cast<double>(cached_tokens) / static_cast<double>(prompt_tokens)
                   : 0.0;
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
    int64_t max_completion_tokens = 0;
    double prompt_cost = 0.0;      // USD per token
    double completion_cost = 0.0;
    bool supports_tools = false;
    bool supports_reasoning = false;
    bool supports_images = false;      // "image" among input modalities
    bool supports_audio = false;
    std::vector<std::string> input_modalities;
    std::string description;

    // Cost of a nominal exchange, for ranking cheap vs expensive in the picker.
    double blended_cost_per_mtok() const {
        return (prompt_cost * 0.75 + completion_cost * 0.25) * 1e6;
    }
};

class Client;   // defined below; ModelCatalog only needs a reference

// Cached lookup of the /models catalogue. The list is ~350 entries and changes
// rarely, so fetching it on every run would be a needless round trip on a slow
// machine.
class ModelCatalog {
public:
    // Loads from disk cache if it is present and younger than `max_age_s`,
    // otherwise fetches. Returns false only if both fail.
    bool load(Client& client, std::string* error, bool force_refresh = false,
              int64_t max_age_s = 24 * 3600);

    // Metadata for one id. Returns null when unknown (an id the user typed, or
    // a catalogue we could not fetch).
    const ModelInfo* find(const std::string& id) const;

    const std::vector<ModelInfo>& all() const { return models_; }
    bool empty() const { return models_.empty(); }

    // Substring match over id and name, ranked so that exact and prefix
    // matches come first.
    std::vector<const ModelInfo*> search(const std::string& query,
                                         size_t limit = 200) const;

    // Per provider: one shared file would serve OpenRouter's catalogue to
    // DeepSeek, which is both wrong and invisible until someone wonders why
    // the model list looks nothing like the service they selected.
    static std::string cache_path(const std::string& provider_id);

    // Context window to assume when the catalogue has no entry for a model.
    static constexpr int64_t kUnknownContext = 128000;

    // Effective context for an id, falling back to kUnknownContext.
    int64_t context_for(const std::string& id) const;

private:
    std::vector<ModelInfo> models_;
    std::map<std::string, size_t> by_id_;

    // Whose catalogue this is. Set by load().
    std::string provider_id_ = "openrouter";

    void reindex();
    bool read_cache(int64_t max_age_s);
    void write_cache() const;
};

// Model ids worth putting in front of the user rather than making them recall,
// ordered by ascending capability and cost. Provider-specific: OpenRouter's ids
// are meaningless to DeepSeek and vice versa.
std::vector<std::string> favorite_models(const std::string& provider_id);

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
    void set_api_key(const std::string& k) { cfg_.api_key = k; }
    void set_config(const Config& c) { cfg_ = c; }

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

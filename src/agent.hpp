// agent.hpp -- the model/tool loop. Front-end agnostic: the TUI and the
// headless runner both drive this same class and differ only in the callbacks
// they install.
#pragma once

#include "common.hpp"
#include "openrouter.hpp"
#include "session.hpp"
#include "tools.hpp"

#include <atomic>
#include <mutex>

namespace ppcode {

// A tool result beginning with this marker, followed by a path and a newline,
// asks the agent loop to show that image to the model. Used by the screenshot
// tool; the image is delivered as a separate user message because tool results
// are text-only on most providers.
inline constexpr const char* kAttachImageMarker = "PPCODE_ATTACH_IMAGE:";

class Agent {
public:
    struct Events {
        // Streaming assistant text, delta by delta.
        std::function<void(const std::string&)> on_text;
        std::function<void(const std::string&)> on_reasoning;

        // A tool is about to run / has finished.
        std::function<void(const ToolCall&)> on_tool_start;
        std::function<void(const ToolCall&, const ToolResult&)> on_tool_done;

        // Approval gate for mutating and executing tools. If unset, such tools
        // are refused unless the config says yolo.
        std::function<bool(const std::string&, ToolKind, const ToolPreview&)> approve;

        // Human-facing progress ("thinking", "running bash", ...).
        std::function<void(const std::string&)> on_status;
        std::function<void(const std::string&)> on_error;

        // Fired when the assistant produces a complete turn (before tools run).
        std::function<void(const Message&)> on_assistant_message;
    };

    struct RunResult {
        bool ok = false;
        std::string error;
        Usage usage;             // summed across every round in this run
        int rounds = 0;          // model calls made
        int tool_calls = 0;
        std::string final_text;  // last assistant text
        bool cancelled = false;
        bool hit_turn_limit = false;
        bool hit_cost_limit = false;
    };

    Agent(Client& client, ToolRegistry& tools, const Config& cfg);

    // Submit one user message and run until the model stops calling tools.
    RunResult run(const std::string& user_input, const Events& ev,
                  std::atomic<bool>* cancel = nullptr);

    // As above, but with a pre-built message -- used when the turn carries
    // attachments as content parts rather than plain text.
    RunResult run(const Message& user_message, const Events& ev,
                  std::atomic<bool>* cancel = nullptr);

    // Continue without adding a user message (used after /compact or to resume).
    RunResult run_continuation(const Events& ev, std::atomic<bool>* cancel = nullptr);

    // Steering: additional user input supplied while a turn is already running.
    // It is injected between rounds -- after the current round's tool results,
    // before the next model call -- so the model can be redirected without
    // cancelling and losing the work already done. Safe to call from another
    // thread; the TUI calls it from the input loop while the worker runs.
    void queue_steering(const std::string& text);
    bool has_pending_steering() const;

    std::vector<Message>& history() { return history_; }
    const std::vector<Message>& history() const { return history_; }

    void reset();

    // Install a fully assembled system message (see sysprompt::build). Replaces
    // whatever is at the head of the conversation. When set, this wins over the
    // config's prompt and no directory line is appended -- the caller owns the
    // whole text.
    void set_system_prompt(const std::string& text);
    const std::string& system_prompt() const { return system_override_; }

    // Changing the working directory must also rewrite the system prompt --
    // it names the directory, and a stale value sends the model editing files
    // in the wrong tree.
    void set_cwd(const std::string& cwd);
    const std::string& cwd() const { return cwd_; }

    // The model's context window. When set, the conversation is summarised
    // automatically once it approaches the limit, rather than failing the
    // request outright. Zero disables it.
    void set_context_limit(int64_t tokens) { context_limit_ = tokens; }
    int64_t context_limit() const { return context_limit_; }

    // Compact now, regardless of size. Returns false with a reason if the
    // conversation is too short or the summary could not be produced.
    bool compact_now(std::string* summary, std::string* error);

    // Where this session is persisted, and whether to write after every turn.
    void set_session_path(const std::string& path) { session_path_ = path; }
    const std::string& session_path() const { return session_path_; }
    void save_session();

    const Usage& session_usage() const { return session_usage_; }

    // Serialise / restore the conversation, for --resume and /save.
    json to_json() const;
    bool from_json(const json& j, std::string* error);

private:
    Client& client_;
    ToolRegistry& tools_;
    Config cfg_;
    std::vector<Message> history_;
    std::string cwd_;
    Usage session_usage_;
    std::string system_override_;
    bool cost_warned_ = false;

    int64_t context_limit_ = 0;
    std::string session_path_;
    std::string title_;

    mutable std::mutex steering_mu_;
    std::vector<std::string> pending_steering_;
    // Drain anything queued into the conversation. Returns what it injected.
    std::vector<std::string> take_steering();

    RunResult loop(const Events& ev, std::atomic<bool>* cancel);
    void ensure_system_prompt();
};

} // namespace ppcode

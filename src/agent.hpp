// agent.hpp -- the model/tool loop. Front-end agnostic: the TUI and the
// headless runner both drive this same class and differ only in the callbacks
// they install.
#pragma once

#include "common.hpp"
#include "openrouter.hpp"
#include "tools.hpp"

#include <atomic>

namespace ppcode {

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

    RunResult loop(const Events& ev, std::atomic<bool>* cancel);
    void ensure_system_prompt();
};

} // namespace ppcode

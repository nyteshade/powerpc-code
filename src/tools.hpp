// tools.hpp -- the tool registry: builtins plus anything MCP contributes.
#pragma once

#include "common.hpp"
#include "openrouter.hpp"

#include <atomic>
#include <mutex>

namespace ppcode {

struct ToolResult {
    std::string content;      // text handed back to the model
    bool is_error = false;

    static ToolResult ok(const std::string& c)  { return {c, false}; }
    static ToolResult err(const std::string& c) { return {"Error: " + c, true}; }
};

// Whether a tool changes anything. Drives the approval prompt.
enum class ToolKind { Read, Mutate, Execute };

// What the UI shows when asking permission, and what it logs.
struct ToolPreview {
    std::string title;    // e.g. "write_file  src/main.cpp"
    std::string detail;   // e.g. the command, or a diff summary
};

struct ToolContext {
    std::string cwd = ".";
    std::atomic<bool>* cancel = nullptr;

    // Returns true if the call may proceed. Set by the front end; if null,
    // everything is allowed (used by --yolo and by read-only tools).
    std::function<bool(const std::string& name, ToolKind kind,
                       const ToolPreview&)> approve;

    // Optional progress line for long-running tools.
    std::function<void(const std::string&)> note;
};

using ToolHandler = std::function<ToolResult(const json& args, ToolContext& ctx)>;

struct Tool {
    ToolSpec spec;
    ToolKind kind = ToolKind::Read;
    ToolHandler handler;
    // Builds the approval prompt. Optional; a generic one is used otherwise.
    std::function<ToolPreview(const json&)> preview;
    std::string source;    // "builtin" or "mcp:<server>"
};

// A plan the model maintains across turns. Long builds on this hardware span
// many rounds, and a visible checklist is the difference between the model
// keeping its place and starting over.
struct TodoItem {
    std::string text;
    std::string status = "pending";   // pending | in_progress | completed
};

class TodoStore {
public:
    void replace(std::vector<TodoItem> items);
    std::vector<TodoItem> items() const;
    bool empty() const;
    std::string summary() const;      // "2/5 done"
private:
    mutable std::mutex mu_;
    std::vector<TodoItem> items_;
};

class ToolRegistry {
public:
    void add(Tool t);
    void add_builtins();

    // multi_edit, read_many_files, file operations, and the todo list.
    // `todos` may be null to omit the todo tool.
    void add_extra_builtins(TodoStore* todos);

    bool has(const std::string& name) const;
    const Tool* find(const std::string& name) const;

    // Specs for the API request, in registration order.
    std::vector<ToolSpec> specs() const;

    std::vector<std::string> names() const;
    size_t size() const { return order_.size(); }

    // Dispatch, including the approval gate. Unknown tools return an error
    // result rather than throwing, so the model can recover.
    ToolResult call(const std::string& name, const json& args, ToolContext& ctx) const;

    // Drop every tool contributed by a given source (used when an MCP server
    // disconnects).
    void remove_source(const std::string& source);

private:
    std::map<std::string, Tool> tools_;
    std::vector<std::string> order_;
};

// ---------------------------------------------------------------------------
// Shell execution, shared by the bash tool and by MCP stdio spawning.
// ---------------------------------------------------------------------------

struct CommandResult {
    int exit_code = -1;
    std::string output;      // stdout and stderr interleaved
    bool timed_out = false;
    bool spawn_failed = false;
    std::string error;
};

// Runs `cmd` via `sh -c`, capturing combined output, killing it after
// `timeout_ms`. Output is truncated at `max_output` bytes.
CommandResult run_shell(const std::string& cmd, const std::string& cwd,
                        int timeout_ms, size_t max_output = 100 * 1024,
                        std::atomic<bool>* cancel = nullptr);

// Resolve `path` against `cwd`, expanding a leading ~. Does not require the
// path to exist.
std::string resolve_path(const std::string& path, const std::string& cwd);

} // namespace ppcode

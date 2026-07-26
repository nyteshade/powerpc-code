// tools_extra.cpp -- the second tier of builtins.
//
// These exist mostly to save round trips. Every model turn on this hardware
// costs real wall-clock time, so batching several edits or several file reads
// into one call is a meaningful speedup, not just tidiness.
#include "jobs.hpp"
#include "checkpoint.hpp"
#include "tools.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode {

// ---------------------------------------------------------------------------
// TodoStore
// ---------------------------------------------------------------------------

void TodoStore::replace(std::vector<TodoItem> items) {
    std::lock_guard<std::mutex> lk(mu_);
    items_ = std::move(items);
}

std::vector<TodoItem> TodoStore::items() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_;
}

bool TodoStore::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_.empty();
}

std::string TodoStore::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (items_.empty()) return "";
    size_t done = 0;
    for (const TodoItem& t : items_) if (t.status == "completed") done++;
    return std::to_string(done) + "/" + std::to_string(items_.size()) + " done";
}

// ---------------------------------------------------------------------------

namespace {

// ---- multi_edit -----------------------------------------------------------

ToolResult tool_multi_edit(const json& a, ToolContext& ctx) {
    std::string path = jstr(a, "path");
    if (path.empty()) return ToolResult::err("'path' is required");

    const json* edits = jptr(a, "edits");
    if (!edits || !edits->is_array() || edits->empty())
        return ToolResult::err("'edits' must be a non-empty array");

    std::string full = resolve_path(path, ctx.cwd);
    std::string text, rerr;
    if (!read_file_text(full, &text, &rerr)) return ToolResult::err(rerr);

    // Validate every edit against the evolving text before writing anything, so
    // a bad edit in the middle cannot leave the file half-modified.
    std::string working = text;
    size_t applied = 0;
    for (size_t i = 0; i < edits->size(); i++) {
        const json& e = (*edits)[i];
        const json* oldj = jptr(e, "old_string");
        const json* newj = jptr(e, "new_string");
        std::string where = "edits[" + std::to_string(i) + "]";
        if (!oldj || !oldj->is_string())
            return ToolResult::err(where + ": 'old_string' is required");
        if (!newj || !newj->is_string())
            return ToolResult::err(where + ": 'new_string' is required");

        std::string from = oldj->get<std::string>();
        std::string to = newj->get<std::string>();
        bool all = jbool(e, "replace_all", false);

        if (from.empty())
            return ToolResult::err(where + ": 'old_string' must not be empty");
        if (from == to)
            return ToolResult::err(where + ": old_string and new_string are identical");

        size_t n = count_occurrences(working, from);
        if (n == 0)
            return ToolResult::err(
                where + ": old_string not found. Note that earlier edits in this "
                "call have already been applied to the working copy, so match "
                "against the text as it will be at that point. Nothing was "
                "written.");
        if (n > 1 && !all)
            return ToolResult::err(where + ": old_string appears " +
                                   std::to_string(n) + " times; add context or set "
                                   "replace_all. Nothing was written.");

        if (all) {
            std::string out;
            size_t pos = 0;
            while (true) {
                size_t p = working.find(from, pos);
                if (p == std::string::npos) { out += working.substr(pos); break; }
                out += working.substr(pos, p - pos);
                out += to;
                pos = p + from.size();
                applied++;
            }
            working = out;
        } else {
            replace_first(working, from, to);
            applied++;
        }
    }

    checkpoint::store().record_before(full, "multi_edit");
    std::string werr;
    if (!write_file_text(full, working, &werr)) return ToolResult::err(werr);
    std::string diff = checkpoint::store().record_after(full);

    if (!diff.empty())
        return ToolResult::ok("Applied " + std::to_string(edits->size()) +
                              " edits to " + path + "\n\n" + elide(diff, 4000));
    return ToolResult::ok("Applied " + std::to_string(edits->size()) + " edit" +
                          (edits->size() == 1 ? "" : "s") + " (" +
                          std::to_string(applied) + " replacement" +
                          (applied == 1 ? "" : "s") + ") to " + path);
}

// ---- read_many_files ------------------------------------------------------

ToolResult tool_read_many_files(const json& a, ToolContext& ctx) {
    const json* paths = jptr(a, "paths");
    if (!paths || !paths->is_array() || paths->empty())
        return ToolResult::err("'paths' must be a non-empty array");
    if (paths->size() > 30)
        return ToolResult::err("too many paths (max 30 per call)");

    int64_t max_lines = jint(a, "max_lines_each", 400);
    if (max_lines <= 0 || max_lines > 3000) max_lines = 400;

    std::string out;
    size_t ok_count = 0;
    for (const json& p : *paths) {
        if (!p.is_string()) continue;
        std::string rel = p.get<std::string>();
        std::string full = resolve_path(rel, ctx.cwd);

        out += "===== " + rel + " =====\n";
        std::string text, err;
        if (!read_file_text(full, &text, &err)) {
            out += "(cannot read: " + err + ")\n\n";
            continue;
        }
        std::vector<std::string> lines = split(text, '\n');
        bool truncated = static_cast<int64_t>(lines.size()) > max_lines;
        if (truncated) lines.resize(static_cast<size_t>(max_lines));

        char num[24];
        for (size_t i = 0; i < lines.size(); i++) {
            std::snprintf(num, sizeof(num), "%6zu\t", i + 1);
            out += num;
            out += lines[i];
            out += "\n";
        }
        if (truncated)
            out += "[truncated at " + std::to_string(max_lines) +
                   " lines; use read_file with an offset for the rest]\n";
        out += "\n";
        ok_count++;
    }
    if (ok_count == 0) return ToolResult::err("none of the paths could be read");
    return ToolResult::ok(out);
}

// ---- file operations ------------------------------------------------------

ToolResult tool_file_op(const json& a, ToolContext& ctx) {
    std::string op = to_lower(jstr(a, "operation"));
    std::string path = jstr(a, "path");
    if (op.empty()) return ToolResult::err("'operation' is required");
    if (path.empty()) return ToolResult::err("'path' is required");

    std::string full = resolve_path(path, ctx.cwd);
    std::error_code ec;

    if (op == "mkdir") {
        fs::create_directories(full, ec);
        if (ec) return ToolResult::err("mkdir " + path + ": " + ec.message());
        return ToolResult::ok("Created directory " + path);
    }
    if (op == "remove" || op == "delete" || op == "rm") {
        if (!fs::exists(full, ec)) return ToolResult::err(path + " does not exist");
        bool recursive = jbool(a, "recursive", false);
        uintmax_t n = 0;
        if (fs::is_directory(full, ec)) {
            if (!recursive)
                return ToolResult::err(path + " is a directory; pass recursive: true "
                                              "to remove it and its contents");
            n = fs::remove_all(full, ec);
        } else {
            n = fs::remove(full, ec) ? 1 : 0;
        }
        if (ec) return ToolResult::err("remove " + path + ": " + ec.message());
        return ToolResult::ok("Removed " + std::to_string(n) + " item" +
                              (n == 1 ? "" : "s") + " at " + path);
    }

    std::string dest = jstr(a, "destination");
    if (dest.empty())
        return ToolResult::err("'destination' is required for " + op);
    std::string dfull = resolve_path(dest, ctx.cwd);

    fs::path dp(dfull);
    if (dp.has_parent_path()) fs::create_directories(dp.parent_path(), ec);
    ec.clear();

    if (op == "move" || op == "mv" || op == "rename") {
        fs::rename(full, dfull, ec);
        if (ec) {
            // rename fails across filesystems; fall back to copy + remove.
            fs::copy(full, dfull,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                     ec);
            if (ec) return ToolResult::err("move " + path + ": " + ec.message());
            fs::remove_all(full, ec);
        }
        return ToolResult::ok("Moved " + path + " to " + dest);
    }
    if (op == "copy" || op == "cp") {
        fs::copy(full, dfull,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        if (ec) return ToolResult::err("copy " + path + ": " + ec.message());
        return ToolResult::ok("Copied " + path + " to " + dest);
    }
    return ToolResult::err("unknown operation '" + op +
                           "' (use mkdir, remove, move, or copy)");
}

// ---- todo_write -----------------------------------------------------------

bool valid_status(const std::string& s) {
    return s == "pending" || s == "in_progress" || s == "completed";
}

} // namespace

void ToolRegistry::add_extra_builtins(TodoStore* todos) {
    {
        Tool t;
        t.spec.name = "multi_edit";
        t.spec.description =
            "Apply several exact-string edits to one file in a single call. Edits "
            "are applied in order, each seeing the result of the previous one. If "
            "any edit fails to match, nothing is written at all. Prefer this over "
            "repeated edit_file calls -- each round trip is slow on this machine.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "path":  {"type": "string", "description": "File to edit."},
                "edits": {
                    "type": "array",
                    "description": "Edits to apply in order.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "old_string":  {"type": "string",  "description": "Exact text to find."},
                            "new_string":  {"type": "string",  "description": "Replacement text."},
                            "replace_all": {"type": "boolean", "description": "Replace every occurrence."}
                        },
                        "required": ["old_string", "new_string"]
                    }
                }
            },
            "required": ["path", "edits"]
        })");
        t.kind = ToolKind::Mutate;
        t.handler = tool_multi_edit;
        t.source = "builtin";
        t.preview = [](const json& a) {
            const json* e = jptr(a, "edits");
            size_t n = (e && e->is_array()) ? e->size() : 0;
            return ToolPreview{"multi_edit  " + jstr(a, "path"),
                               std::to_string(n) + " edits"};
        };
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "read_many_files";
        t.spec.description =
            "Read several files in one call. Use this when exploring a codebase "
            "instead of issuing read_file repeatedly.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "paths":          {"type": "array", "items": {"type": "string"},
                                   "description": "Paths to read, up to 30."},
                "max_lines_each": {"type": "integer",
                                   "description": "Cap lines returned per file. Default 400."}
            },
            "required": ["paths"]
        })");
        t.kind = ToolKind::Read;
        t.handler = tool_read_many_files;
        t.source = "builtin";
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "file_op";
        t.spec.description =
            "Create a directory, or move, copy, or remove a file or directory.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "operation":   {"type": "string", "enum": ["mkdir", "move", "copy", "remove"],
                                "description": "What to do."},
                "path":        {"type": "string", "description": "Target path."},
                "destination": {"type": "string", "description": "Required for move and copy."},
                "recursive":   {"type": "boolean", "description": "Required to remove a directory."}
            },
            "required": ["operation", "path"]
        })");
        t.kind = ToolKind::Mutate;
        t.handler = tool_file_op;
        t.source = "builtin";
        t.preview = [](const json& a) {
            std::string d = jstr(a, "destination");
            return ToolPreview{"file_op  " + jstr(a, "operation"),
                               jstr(a, "path") + (d.empty() ? "" : " -> " + d)};
        };
        add(std::move(t));
    }

    if (todos) {
        Tool t;
        t.spec.name = "todo_write";
        t.spec.description =
            "Record or update your plan as a checklist. Replaces the whole list, "
            "so send every item each time with its current status. Worth using "
            "for any task spanning several steps -- it survives across turns and "
            "is shown to the user.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "items": {
                    "type": "array",
                    "description": "The full checklist, in order.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "text":   {"type": "string", "description": "What the step is."},
                            "status": {"type": "string", "enum": ["pending", "in_progress", "completed"]}
                        },
                        "required": ["text", "status"]
                    }
                }
            },
            "required": ["items"]
        })");
        t.kind = ToolKind::Read;    // bookkeeping, nothing to approve
        t.source = "builtin";
        t.handler = [todos](const json& a, ToolContext&) -> ToolResult {
            const json* items = jptr(a, "items");
            if (!items || !items->is_array())
                return ToolResult::err("'items' must be an array");

            std::vector<TodoItem> parsed;
            int in_progress = 0;
            for (const json& it : *items) {
                TodoItem item;
                item.text = jstr(it, "text");
                item.status = jstr(it, "status", "pending");
                if (item.text.empty()) continue;
                if (!valid_status(item.status))
                    return ToolResult::err("invalid status '" + item.status +
                                           "' (use pending, in_progress, or completed)");
                if (item.status == "in_progress") in_progress++;
                parsed.push_back(std::move(item));
            }
            if (parsed.empty()) return ToolResult::err("no valid items");
            if (in_progress > 1)
                return ToolResult::err("only one item may be in_progress at a time");

            todos->replace(parsed);

            std::string out = "Plan updated (" + todos->summary() + "):\n";
            for (const TodoItem& i : todos->items()) {
                const char* mark = i.status == "completed"   ? "[x]"
                                 : i.status == "in_progress" ? "[>]" : "[ ]";
                out += std::string("  ") + mark + " " + i.text + "\n";
            }
            return ToolResult::ok(out);
        };
        add(std::move(t));
    }
}

// ---------------------------------------------------------------------------
// Background job tools
// ---------------------------------------------------------------------------

void add_job_tools(ToolRegistry& registry, JobManager& jobs) {
    {
        Tool t;
        t.spec.name = "run_background";
        t.spec.description =
            "Start a long-running shell command detached and return immediately "
            "with a job id. Use this for anything that may outlast the bash tool's "
            "timeout: compiling a large project, `port install`, running a full "
            "test suite. The job keeps running even if ppcode exits, and its "
            "output is captured to a log you can read with job_output.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to run."},
                "cwd":     {"type": "string", "description": "Directory to run in. Defaults to the working directory."}
            },
            "required": ["command"]
        })");
        t.kind = ToolKind::Execute;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"run_background", jstr(a, "command")};
        };
        t.handler = [&jobs](const json& a, ToolContext& ctx) -> ToolResult {
            std::string cmd = jstr(a, "command");
            if (cmd.empty()) return ToolResult::err("'command' is required");
            std::string cwd = jstr(a, "cwd");
            if (cwd.empty()) cwd = ctx.cwd;
            else cwd = resolve_path(cwd, ctx.cwd);

            std::string err;
            int id = jobs.start(cmd, cwd, &err);
            if (id < 0) return ToolResult::err(err);

            Job j;
            std::string log = jobs.get(id, &j) ? j.log_path : "";
            return ToolResult::ok(
                "Started job " + std::to_string(id) + " (pid " +
                std::to_string(j.pid) + ").\nLog: " + log +
                "\nCheck on it with job_output {\"id\": " + std::to_string(id) +
                "}. Do not poll tightly -- give it time to make progress.");
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "job_list";
        t.spec.description =
            "List background jobs with their status, elapsed time and exit code. "
            "Includes jobs started by earlier ppcode sessions.";
        t.spec.parameters = json::parse(R"({"type": "object", "properties": {}})");
        t.kind = ToolKind::Read;
        t.source = "builtin";
        t.handler = [&jobs](const json&, ToolContext&) -> ToolResult {
            std::vector<Job> all = jobs.list();
            if (all.empty()) return ToolResult::ok("No background jobs.");
            std::string out;
            for (const Job& j : all) {
                out += "job " + std::to_string(j.id) + "  ";
                if (j.running) out += "RUNNING  " + j.elapsed();
                else out += "exited " + std::to_string(j.exit_code) + "  after " + j.elapsed();
                out += "\n  " + elide(j.command, 140) + "\n";
            }
            return ToolResult::ok(out);
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "job_output";
        t.spec.description =
            "Read the tail of a background job's output, and whether it is still "
            "running. This is how you check on a long build.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "id":        {"type": "integer", "description": "Job id from run_background."},
                "max_lines": {"type": "integer", "description": "Lines from the end to show. Default 120."}
            },
            "required": ["id"]
        })");
        t.kind = ToolKind::Read;
        t.source = "builtin";
        t.handler = [&jobs](const json& a, ToolContext&) -> ToolResult {
            int id = static_cast<int>(jint(a, "id", -1));
            if (id < 0) return ToolResult::err("'id' is required");
            int64_t max_lines = jint(a, "max_lines", 120);
            if (max_lines <= 0 || max_lines > 2000) max_lines = 120;

            Job j;
            if (!jobs.get(id, &j))
                return ToolResult::err("no such job: " + std::to_string(id));

            std::string err;
            std::string body = jobs.output(id, static_cast<size_t>(max_lines),
                                           200 * 1024, &err);
            if (!err.empty()) return ToolResult::err(err);

            std::string head = "job " + std::to_string(id) + ": ";
            head += j.running ? ("still running, " + j.elapsed() + " elapsed")
                              : ("finished with exit code " +
                                 std::to_string(j.exit_code) + " after " + j.elapsed());
            return ToolResult::ok(head + "\n\n" + body);
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "job_stop";
        t.spec.description = "Terminate a running background job.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "id":    {"type": "integer", "description": "Job id."},
                "force": {"type": "boolean", "description": "Send SIGKILL instead of SIGTERM."}
            },
            "required": ["id"]
        })");
        t.kind = ToolKind::Execute;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"job_stop", "job " + std::to_string(jint(a, "id", -1))};
        };
        t.handler = [&jobs](const json& a, ToolContext&) -> ToolResult {
            int id = static_cast<int>(jint(a, "id", -1));
            if (id < 0) return ToolResult::err("'id' is required");
            std::string err;
            if (!jobs.stop(id, jbool(a, "force", false), &err))
                return ToolResult::err(err);
            return ToolResult::ok("Signalled job " + std::to_string(id));
        };
        registry.add(std::move(t));
    }
}

} // namespace ppcode

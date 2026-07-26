#include "tools.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ppcode {

// ---------------------------------------------------------------------------
// Shell execution
// ---------------------------------------------------------------------------

static int64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

CommandResult run_shell(const std::string& cmd, const std::string& cwd,
                        int timeout_ms, size_t max_output,
                        std::atomic<bool>* cancel) {
    CommandResult res;

    int fds[2];
    if (pipe(fds) != 0) {
        res.spawn_failed = true;
        res.error = std::string("pipe: ") + std::strerror(errno);
        return res;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        res.spawn_failed = true;
        res.error = std::string("fork: ") + std::strerror(errno);
        return res;
    }

    if (pid == 0) {
        // Child. Put it in its own process group so a timeout can kill the
        // whole tree, not just the shell.
        setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        // Nothing should block reading the terminal.
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }

        if (!cwd.empty() && cwd != ".") {
            if (chdir(cwd.c_str()) != 0) {
                std::fprintf(stderr, "cannot chdir to %s: %s\n", cwd.c_str(),
                             std::strerror(errno));
                _exit(127);
            }
        }
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        std::fprintf(stderr, "exec failed: %s\n", std::strerror(errno));
        _exit(127);
    }

    // Parent.
    close(fds[1]);
    setpgid(pid, pid);   // race-free: both sides call it

    const int64_t deadline = now_ms() + timeout_ms;
    bool truncated = false;
    char buf[8192];

    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) { res.timed_out = true; break; }
        if (cancel && cancel->load()) { res.timed_out = true; break; }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fds[0], &rd);
        // Wake up regularly so cancellation stays responsive.
        struct timeval tv;
        int64_t slice = std::min<int64_t>(remaining, 200);
        tv.tv_sec  = static_cast<long>(slice / 1000);
        tv.tv_usec = static_cast<long>((slice % 1000) * 1000);

        int n = select(fds[0] + 1, &rd, nullptr, nullptr, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;             // timed slice expired, re-check clock

        ssize_t got = read(fds[0], buf, sizeof(buf));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;              // child closed the pipe

        if (res.output.size() < max_output) {
            size_t room = max_output - res.output.size();
            res.output.append(buf, std::min(static_cast<size_t>(got), room));
            if (static_cast<size_t>(got) > room) truncated = true;
        } else {
            truncated = true;
        }
    }

    close(fds[0]);

    if (res.timed_out) {
        kill(-pid, SIGKILL);              // whole process group
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    if (WIFEXITED(status))        res.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.exit_code = 128 + WTERMSIG(status);

    if (truncated) res.output += "\n[output truncated]";
    return res;
}

std::string resolve_path(const std::string& path, const std::string& cwd) {
    std::string p = expand_user(path);
    if (p.empty()) return cwd;
    if (p[0] == '/') return p;
    if (cwd.empty() || cwd == ".") return p;
    return cwd + "/" + p;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

void ToolRegistry::add(Tool t) {
    std::string name = t.spec.name;
    if (!tools_.count(name)) order_.push_back(name);
    tools_[name] = std::move(t);
}

bool ToolRegistry::has(const std::string& name) const { return tools_.count(name) > 0; }

const Tool* ToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<ToolSpec> ToolRegistry::specs() const {
    std::vector<ToolSpec> out;
    for (const std::string& n : order_) {
        auto it = tools_.find(n);
        if (it != tools_.end()) out.push_back(it->second.spec);
    }
    return out;
}

std::vector<std::string> ToolRegistry::names() const { return order_; }

void ToolRegistry::remove_source(const std::string& source) {
    for (auto it = tools_.begin(); it != tools_.end();) {
        if (it->second.source == source) {
            std::string n = it->first;
            it = tools_.erase(it);
            order_.erase(std::remove(order_.begin(), order_.end(), n), order_.end());
        } else {
            ++it;
        }
    }
}

ToolResult ToolRegistry::call(const std::string& name, const json& args,
                              ToolContext& ctx) const {
    const Tool* t = find(name);
    if (!t) {
        std::string known = join(names(), ", ");
        return ToolResult::err("unknown tool '" + name + "'. Available: " + known);
    }

    // Approval gate. Read-only tools bypass it; the front end decides policy
    // for the rest by installing (or not installing) ctx.approve.
    if (t->kind != ToolKind::Read && ctx.approve) {
        ToolPreview pv = t->preview ? t->preview(args)
                                    : ToolPreview{name, json_preview(args, 200)};
        if (!ctx.approve(name, t->kind, pv))
            return ToolResult::err("denied by user");
    }

    try {
        return t->handler(args, ctx);
    } catch (const std::exception& e) {
        return ToolResult::err(std::string("tool threw: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

namespace {

std::string arg_str(const json& a, const std::string& k, const std::string& def = "") {
    return jstr(a, k, def);
}

// Render with 1-based line numbers so the model can refer to them.
std::string number_lines(const std::string& text, int64_t start_line) {
    std::vector<std::string> lines = split(text, '\n');
    std::string out;
    char num[24];
    for (size_t i = 0; i < lines.size(); i++) {
        std::snprintf(num, sizeof(num), "%6lld\t",
                      static_cast<long long>(start_line + static_cast<int64_t>(i)));
        out += num;
        out += lines[i];
        out += "\n";
    }
    return out;
}

bool looks_binary(const std::string& s) {
    size_t n = std::min<size_t>(s.size(), 8000);
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\0') return true;
    return false;
}

// ---- read_file ------------------------------------------------------------

ToolResult tool_read_file(const json& a, ToolContext& ctx) {
    std::string path = arg_str(a, "path");
    if (path.empty()) return ToolResult::err("'path' is required");
    std::string full = resolve_path(path, ctx.cwd);

    std::error_code ec;
    if (fs::is_directory(full, ec))
        return ToolResult::err(path + " is a directory; use list_dir");

    std::string text, rerr;
    if (!read_file_text(full, &text, &rerr)) return ToolResult::err(rerr);

    if (looks_binary(text))
        return ToolResult::err(path + " looks like a binary file");

    int64_t offset = jint(a, "offset", 1);      // 1-based
    int64_t limit  = jint(a, "limit", 2000);
    if (offset < 1) offset = 1;

    std::vector<std::string> lines = split(text, '\n');
    int64_t total = static_cast<int64_t>(lines.size());

    if (offset > total)
        return ToolResult::ok("(file has " + std::to_string(total) +
                              " lines; offset " + std::to_string(offset) + " is past the end)");

    int64_t end = std::min<int64_t>(total, offset - 1 + limit);
    std::string slice;
    for (int64_t i = offset - 1; i < end; i++) {
        slice += lines[static_cast<size_t>(i)];
        if (i + 1 < end) slice += "\n";
    }

    std::string out = number_lines(slice, offset);
    if (end < total)
        out += "\n[showing lines " + std::to_string(offset) + "-" + std::to_string(end) +
               " of " + std::to_string(total) + "; pass offset to read more]";
    return ToolResult::ok(out);
}

// ---- write_file -----------------------------------------------------------

ToolResult tool_write_file(const json& a, ToolContext& ctx) {
    std::string path = arg_str(a, "path");
    if (path.empty()) return ToolResult::err("'path' is required");
    const json* c = jptr(a, "content");
    if (!c || !c->is_string()) return ToolResult::err("'content' (string) is required");

    std::string full = resolve_path(path, ctx.cwd);
    std::string content = c->get<std::string>();

    std::error_code ec;
    fs::path p(full);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

    std::string werr;
    if (!write_file_text(full, content, &werr)) return ToolResult::err(werr);

    size_t nlines = split(content, '\n').size();
    return ToolResult::ok("Wrote " + std::to_string(content.size()) + " bytes (" +
                          std::to_string(nlines) + " lines) to " + path);
}

// ---- edit_file ------------------------------------------------------------

ToolResult tool_edit_file(const json& a, ToolContext& ctx) {
    std::string path = arg_str(a, "path");
    if (path.empty()) return ToolResult::err("'path' is required");
    const json* oldj = jptr(a, "old_string");
    const json* newj = jptr(a, "new_string");
    if (!oldj || !oldj->is_string()) return ToolResult::err("'old_string' is required");
    if (!newj || !newj->is_string()) return ToolResult::err("'new_string' is required");

    std::string old_s = oldj->get<std::string>();
    std::string new_s = newj->get<std::string>();
    bool all = jbool(a, "replace_all", false);

    if (old_s == new_s)
        return ToolResult::err("old_string and new_string are identical");
    if (old_s.empty())
        return ToolResult::err("old_string must not be empty; use write_file to create a file");

    std::string full = resolve_path(path, ctx.cwd);
    std::string text, rerr;
    if (!read_file_text(full, &text, &rerr)) return ToolResult::err(rerr);

    size_t n = count_occurrences(text, old_s);
    if (n == 0)
        return ToolResult::err("old_string not found in " + path +
                               ". Read the file and match its exact text, including indentation.");
    if (n > 1 && !all)
        return ToolResult::err("old_string appears " + std::to_string(n) + " times in " +
                               path + ". Add more surrounding context to make it unique, "
                               "or pass replace_all: true.");

    size_t replaced = 0;
    if (all) {
        std::string out;
        size_t pos = 0;
        while (true) {
            size_t p = text.find(old_s, pos);
            if (p == std::string::npos) { out += text.substr(pos); break; }
            out += text.substr(pos, p - pos);
            out += new_s;
            pos = p + old_s.size();
            replaced++;
        }
        text = out;
    } else {
        replace_first(text, old_s, new_s);
        replaced = 1;
    }

    std::string werr;
    if (!write_file_text(full, text, &werr)) return ToolResult::err(werr);

    return ToolResult::ok("Replaced " + std::to_string(replaced) +
                          (replaced == 1 ? " occurrence in " : " occurrences in ") + path);
}

// ---- list_dir -------------------------------------------------------------

ToolResult tool_list_dir(const json& a, ToolContext& ctx) {
    std::string path = arg_str(a, "path", ".");
    std::string full = resolve_path(path, ctx.cwd);

    std::error_code ec;
    if (!fs::exists(full, ec)) return ToolResult::err(path + " does not exist");
    if (!fs::is_directory(full, ec)) return ToolResult::err(path + " is not a directory");

    std::vector<std::string> dirs, files;
    for (const auto& e : fs::directory_iterator(full, ec)) {
        std::string name = e.path().filename().string();
        if (name == "." || name == "..") continue;
        std::error_code e2;
        if (e.is_directory(e2)) {
            dirs.push_back(name + "/");
        } else {
            uintmax_t sz = e.file_size(e2);
            char line[512];
            std::snprintf(line, sizeof(line), "%-40s %8llu", name.c_str(),
                          static_cast<unsigned long long>(e2 ? 0 : sz));
            files.push_back(line);
        }
    }
    if (ec) return ToolResult::err("cannot read " + path + ": " + ec.message());

    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    std::string out = full + ":\n";
    for (const std::string& d : dirs)  out += "  " + d + "\n";
    for (const std::string& f : files) out += "  " + f + "\n";
    if (dirs.empty() && files.empty()) out += "  (empty)\n";
    return ToolResult::ok(out);
}

// ---- glob -----------------------------------------------------------------

// Simple recursive-descent matcher supporting * ? and ** across separators.
bool glob_match(const std::string& pat, const std::string& str, size_t pi = 0,
                size_t si = 0) {
    while (pi < pat.size()) {
        if (pat[pi] == '*') {
            bool dstar = (pi + 1 < pat.size() && pat[pi + 1] == '*');
            if (dstar) {
                size_t next = pi + 2;
                if (next < pat.size() && pat[next] == '/') next++;
                for (size_t k = si; k <= str.size(); k++)
                    if (glob_match(pat, str, next, k)) return true;
                return false;
            }
            for (size_t k = si; k <= str.size(); k++) {
                if (glob_match(pat, str, pi + 1, k)) return true;
                if (k < str.size() && str[k] == '/') break;   // * stops at /
            }
            return false;
        }
        if (si >= str.size()) return false;
        if (pat[pi] != '?' && pat[pi] != str[si]) return false;
        pi++;
        si++;
    }
    return si == str.size();
}

bool skip_dir(const std::string& name) {
    return name == ".git" || name == "build" || name == "node_modules" ||
           name == ".svn" || name == "CVS";
}

ToolResult tool_glob(const json& a, ToolContext& ctx) {
    std::string pattern = arg_str(a, "pattern");
    if (pattern.empty()) return ToolResult::err("'pattern' is required");
    std::string root = resolve_path(arg_str(a, "path", "."), ctx.cwd);
    int64_t limit = jint(a, "limit", 200);

    std::error_code ec;
    if (!fs::is_directory(root, ec)) return ToolResult::err(root + " is not a directory");

    std::vector<std::string> hits;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        if (ctx.cancel && ctx.cancel->load()) break;
        const fs::path& p = it->path();
        std::string name = p.filename().string();

        std::error_code e2;
        if (it->is_directory(e2)) {
            if (skip_dir(name)) { it.disable_recursion_pending(); }
            continue;
        }
        std::string rel = fs::relative(p, root, e2).string();
        if (e2) rel = p.string();
        if (glob_match(pattern, rel) || glob_match(pattern, name))
            hits.push_back(rel);
        if (static_cast<int64_t>(hits.size()) >= limit) break;
    }

    std::sort(hits.begin(), hits.end());
    if (hits.empty()) return ToolResult::ok("No files matched " + pattern);
    std::string out = join(hits, "\n");
    if (static_cast<int64_t>(hits.size()) >= limit)
        out += "\n[stopped at " + std::to_string(limit) + " matches]";
    return ToolResult::ok(out);
}

// ---- grep -----------------------------------------------------------------

ToolResult tool_grep(const json& a, ToolContext& ctx) {
    std::string pattern = arg_str(a, "pattern");
    if (pattern.empty()) return ToolResult::err("'pattern' is required");
    std::string root = resolve_path(arg_str(a, "path", "."), ctx.cwd);
    std::string include = arg_str(a, "include");
    int64_t limit = jint(a, "limit", 100);
    bool icase = jbool(a, "ignore_case", false);

    std::string needle = icase ? to_lower(pattern) : pattern;

    std::error_code ec;
    std::vector<std::string> out_lines;

    auto scan_file = [&](const fs::path& p) {
        std::string text;
        if (!read_file_text(p.string(), &text, nullptr)) return;
        if (looks_binary(text)) return;
        std::error_code e2;
        std::string rel = fs::relative(p, root, e2).string();
        if (e2) rel = p.string();

        std::vector<std::string> lines = split(text, '\n');
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& raw = lines[i];
            const std::string hay = icase ? to_lower(raw) : raw;
            if (hay.find(needle) == std::string::npos) continue;
            out_lines.push_back(rel + ":" + std::to_string(i + 1) + ": " +
                                elide(trim(raw), 200));
            if (static_cast<int64_t>(out_lines.size()) >= limit) return;
        }
    };

    if (fs::is_regular_file(root, ec)) {
        scan_file(root);
    } else {
        auto it = fs::recursive_directory_iterator(
            root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; it != end && !ec; it.increment(ec)) {
            if (ctx.cancel && ctx.cancel->load()) break;
            if (static_cast<int64_t>(out_lines.size()) >= limit) break;
            std::error_code e2;
            if (it->is_directory(e2)) {
                if (skip_dir(it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            if (!include.empty() &&
                !glob_match(include, it->path().filename().string()))
                continue;
            scan_file(it->path());
        }
    }

    if (out_lines.empty()) return ToolResult::ok("No matches for " + pattern);
    std::string out = join(out_lines, "\n");
    if (static_cast<int64_t>(out_lines.size()) >= limit)
        out += "\n[stopped at " + std::to_string(limit) + " matches]";
    return ToolResult::ok(out);
}

// ---- bash -----------------------------------------------------------------

ToolResult tool_bash(const json& a, ToolContext& ctx) {
    std::string cmd = arg_str(a, "command");
    if (cmd.empty()) return ToolResult::err("'command' is required");
    int64_t timeout = jint(a, "timeout_ms", 120000);
    if (timeout <= 0 || timeout > 600000) timeout = 120000;

    if (ctx.note) ctx.note("$ " + elide(cmd, 120));

    CommandResult r = run_shell(cmd, ctx.cwd, static_cast<int>(timeout),
                                100 * 1024, ctx.cancel);
    if (r.spawn_failed) return ToolResult::err(r.error);

    std::string out = r.output;
    if (out.empty()) out = "(no output)";
    if (r.timed_out)
        return ToolResult::err("command timed out after " + std::to_string(timeout) +
                               "ms and was killed\n" + out);
    if (r.exit_code != 0)
        out += "\n[exit code " + std::to_string(r.exit_code) + "]";
    return ToolResult::ok(out);
}

} // namespace

// ---------------------------------------------------------------------------

void ToolRegistry::add_builtins() {
    {
        Tool t;
        t.spec.name = "read_file";
        t.spec.description =
            "Read a text file from disk. Returns the contents with 1-based line "
            "numbers. Use offset and limit for large files.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "path":   {"type": "string",  "description": "File path, absolute or relative to the working directory."},
                "offset": {"type": "integer", "description": "First line to read, 1-based. Default 1."},
                "limit":  {"type": "integer", "description": "Maximum lines to return. Default 2000."}
            },
            "required": ["path"]
        })");
        t.kind = ToolKind::Read;
        t.handler = tool_read_file;
        t.source = "builtin";
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "write_file";
        t.spec.description =
            "Write a file, creating it or replacing its entire contents. Parent "
            "directories are created as needed. To change part of an existing "
            "file, prefer edit_file.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "path":    {"type": "string", "description": "File path to write."},
                "content": {"type": "string", "description": "Full file contents."}
            },
            "required": ["path", "content"]
        })");
        t.kind = ToolKind::Mutate;
        t.handler = tool_write_file;
        t.preview = [](const json& a) {
            std::string c = jstr(a, "content");
            size_t lines = split(c, '\n').size();
            return ToolPreview{"write_file  " + jstr(a, "path"),
                               std::to_string(c.size()) + " bytes, " +
                                   std::to_string(lines) + " lines"};
        };
        t.source = "builtin";
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "edit_file";
        t.spec.description =
            "Replace an exact string in a file. old_string must match the file "
            "byte for byte, including indentation, and must be unique unless "
            "replace_all is set. Read the file first.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "path":        {"type": "string",  "description": "File to edit."},
                "old_string":  {"type": "string",  "description": "Exact text to find."},
                "new_string":  {"type": "string",  "description": "Replacement text."},
                "replace_all": {"type": "boolean", "description": "Replace every occurrence. Default false."}
            },
            "required": ["path", "old_string", "new_string"]
        })");
        t.kind = ToolKind::Mutate;
        t.handler = tool_edit_file;
        t.preview = [](const json& a) {
            return ToolPreview{"edit_file  " + jstr(a, "path"),
                               "- " + json_preview(jstr(a, "old_string"), 70) + "\n" +
                               "+ " + json_preview(jstr(a, "new_string"), 70)};
        };
        t.source = "builtin";
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "list_dir";
        t.spec.description = "List the entries of a directory with file sizes.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Directory to list. Default is the working directory."}
            }
        })");
        t.kind = ToolKind::Read;
        t.handler = tool_list_dir;
        t.source = "builtin";
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "glob";
        t.spec.description =
            "Find files by name pattern, recursively. Supports * ? and **. "
            "Skips .git, build, and node_modules.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "pattern": {"type": "string",  "description": "Glob such as **/*.cpp or Makefile."},
                "path":    {"type": "string",  "description": "Directory to search from. Default is the working directory."},
                "limit":   {"type": "integer", "description": "Maximum matches. Default 200."}
            },
            "required": ["pattern"]
        })");
        t.kind = ToolKind::Read;
        t.handler = tool_glob;
        t.source = "builtin";
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "grep";
        t.spec.description =
            "Search file contents for a literal substring, recursively. Returns "
            "path:line: text. This is a plain substring search, not a regex.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "pattern":     {"type": "string",  "description": "Literal text to find."},
                "path":        {"type": "string",  "description": "File or directory to search. Default is the working directory."},
                "include":     {"type": "string",  "description": "Only search files whose name matches this glob, e.g. *.cpp"},
                "ignore_case": {"type": "boolean", "description": "Case-insensitive search."},
                "limit":       {"type": "integer", "description": "Maximum matching lines. Default 100."}
            },
            "required": ["pattern"]
        })");
        t.kind = ToolKind::Read;
        t.handler = tool_grep;
        t.source = "builtin";
        add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "bash";
        t.spec.description =
            "Run a shell command with /bin/sh and return its combined output. "
            "This machine is Mac OS X 10.5 (Leopard) on PowerPC: coreutils are "
            "the BSD versions, so avoid GNU-only flags. MacPorts tools live in "
            "/opt/local/bin.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "command":    {"type": "string",  "description": "Shell command to run."},
                "timeout_ms": {"type": "integer", "description": "Kill the command after this long. Default 120000, max 600000."}
            },
            "required": ["command"]
        })");
        t.kind = ToolKind::Execute;
        t.handler = tool_bash;
        t.preview = [](const json& a) {
            return ToolPreview{"bash", jstr(a, "command")};
        };
        t.source = "builtin";
        add(std::move(t));
    }
}

} // namespace ppcode

#include "commands.hpp"

#include "yaml.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::commands {

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

std::string Command::expand(const std::string& args) const {
    std::string out = body;
    bool substituted = false;

    // $ARGUMENTS first, so a body using it does not also collect the appended
    // copy below.
    while (true) {
        size_t p = out.find("$ARGUMENTS");
        if (p == std::string::npos) break;
        out.replace(p, std::strlen("$ARGUMENTS"), args);
        substituted = true;
    }

    // $1 .. $9, whitespace-separated.
    std::vector<std::string> words;
    {
        size_t i = 0;
        while (i < args.size()) {
            while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i]))) i++;
            if (i >= args.size()) break;
            size_t start = i;
            while (i < args.size() && !std::isspace(static_cast<unsigned char>(args[i]))) i++;
            words.push_back(args.substr(start, i - start));
        }
    }
    for (int n = 1; n <= 9; n++) {
        std::string token = "$" + std::to_string(n);
        std::string value = (static_cast<size_t>(n) <= words.size())
                                ? words[static_cast<size_t>(n) - 1]
                                : "";
        while (true) {
            size_t p = out.find(token);
            if (p == std::string::npos) break;
            out.replace(p, token.size(), value);
            substituted = true;
        }
    }

    // A command with no placeholders is still useful: append whatever was
    // typed rather than silently dropping it.
    if (!substituted && !trim(args).empty()) out += "\n\n" + args;
    return out;
}

std::vector<std::string> command_dirs() {
    std::vector<std::string> dirs;
    if (const char* e = std::getenv("PPCODE_COMMANDS_DIR"); e && *e) dirs.push_back(e);
    if (const char* h = std::getenv("HOME"); h && *h)
        dirs.push_back(std::string(h) + "/.config/ppcode/commands");
    dirs.push_back(".ppcode/commands");
    return dirs;
}

std::vector<Command> load(std::vector<std::string>* warnings) {
    std::vector<Command> out;
    std::vector<std::string> seen;

    for (const std::string& dir : command_dirs()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            if (ends_with(to_lower(e.path().filename().string()), ".md"))
                files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());

        for (const fs::path& f : files) {
            std::string text, err;
            if (!read_file_text(f.string(), &text, &err)) {
                if (warnings) warnings->push_back("command: " + err);
                continue;
            }

            Command c;
            c.source_path = f.string();
            c.name = f.stem().string();

            std::string front, body, ferr;
            if (!yaml::split_frontmatter(text, &front, &body, &ferr)) {
                if (warnings) warnings->push_back("command " + c.name + ": " + ferr);
                continue;
            }
            c.body = trim(body);

            if (!trim(front).empty()) {
                json meta;
                std::string yerr;
                if (yaml::parse(front, &meta, &yerr) && meta.is_object()) {
                    c.name = jstr(meta, "name", c.name);
                    c.description = jstr(meta, "description");
                    c.model = jstr(meta, "model");
                    if (const json* t = jptr(meta, "tools"); t && t->is_array())
                        for (const json& s : *t)
                            if (s.is_string()) c.allow_tools.push_back(s.get<std::string>());
                } else if (warnings) {
                    warnings->push_back("command " + c.name + " frontmatter: " + yerr);
                }
            }

            if (c.body.empty()) {
                if (warnings)
                    warnings->push_back("command " + c.name +
                                        " has no body; ignored");
                continue;
            }
            // A command must not shadow a built-in, or /help becomes a lie.
            static const char* builtin[] = {
                "help", "model", "models", "tools", "mcp", "env", "term", "jobs",
                "compact", "sessions", "cwd", "yolo", "unicode", "clear", "save",
                "load", "cost", "quit", "exit", "undo", "changes", "todo"};
            bool clash = false;
            for (const char* b : builtin) if (c.name == b) clash = true;
            if (clash) {
                if (warnings)
                    warnings->push_back("command /" + c.name +
                                        " would shadow a built-in command; ignored");
                continue;
            }
            if (std::find(seen.begin(), seen.end(), c.name) != seen.end()) continue;
            seen.push_back(c.name);
            out.push_back(std::move(c));
        }
    }
    return out;
}

const Command* find(const std::vector<Command>& cmds, const std::string& name) {
    for (const Command& c : cmds)
        if (c.name == name) return &c;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

std::vector<Hook> load_hooks(const json& config, std::vector<std::string>* warnings) {
    std::vector<Hook> out;
    const json* arr = jptr(config, "hooks");
    if (!arr) return out;
    if (!arr->is_array()) {
        if (warnings) warnings->push_back("'hooks' must be an array; ignored");
        return out;
    }

    for (const json& h : *arr) {
        if (!h.is_object()) continue;
        Hook hook;
        hook.event = to_lower(jstr(h, "event"));
        hook.match = jstr(h, "match", jstr(h, "tool"));
        hook.command = jstr(h, "command");
        hook.blocking = jbool(h, "blocking", false);
        hook.timeout_ms = static_cast<int>(jint(h, "timeout_ms", 30000));

        if (hook.command.empty()) {
            if (warnings) warnings->push_back("hook with no command; ignored");
            continue;
        }
        if (hook.event != "pre_tool" && hook.event != "post_tool") {
            if (warnings)
                warnings->push_back("hook event must be pre_tool or post_tool "
                                    "(got '" + hook.event + "'); ignored");
            continue;
        }
        if (hook.event == "post_tool" && hook.blocking && warnings)
            warnings->push_back("a post_tool hook cannot block; the tool has "
                                "already run");
        out.push_back(std::move(hook));
    }
    return out;
}

namespace {

bool matches(const Hook& h, const std::string& tool) {
    if (h.match.empty() || h.match == "*") return true;
    // Allow a simple comma-separated list.
    for (const std::string& part : split(h.match, ',')) {
        if (trim(part) == tool) return true;
    }
    return false;
}

std::string env_assignment(const std::string& key, const std::string& value) {
    // Single-quote for the shell, escaping embedded quotes.
    std::string q = "'";
    for (char c : value) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    // Assign and export as separate statements rather than as a `VAR=x cmd`
    // prefix. With the prefix form the variable exists only in the command's
    // environment, and a `$VAR` written in that same command line is expanded
    // by the parent shell first -- so it comes out empty, which is exactly what
    // a hook author would not expect.
    return key + "=" + q + "; export " + key + "; ";
}

} // namespace

HookOutcome run_hooks(const std::vector<Hook>& hooks, const std::string& event,
                      const std::string& tool, const json& args,
                      const std::string& cwd, const std::string& result,
                      bool is_error) {
    HookOutcome out;
    for (const Hook& h : hooks) {
        if (h.event != event) continue;
        if (!matches(h, tool)) continue;

        // The tool's details reach the hook as environment variables so a shell
        // one-liner can act on them without argument parsing.
        std::string prefix;
        prefix += env_assignment("PPCODE_TOOL", tool);
        prefix += env_assignment("PPCODE_ARGS", args.dump());
        prefix += env_assignment("PPCODE_CWD", cwd);
        prefix += env_assignment("PPCODE_EVENT", event);
        if (event == "post_tool") {
            prefix += env_assignment("PPCODE_RESULT", elide(result, 8000));
            prefix += env_assignment("PPCODE_IS_ERROR", is_error ? "1" : "0");
        }

        CommandResult r = run_shell(prefix + h.command, cwd, h.timeout_ms,
                                    64 * 1024, nullptr);
        std::string text = trim(r.output);
        if (!text.empty()) {
            if (!out.message.empty()) out.message += "\n";
            out.message += "[hook] " + elide(text, 800);
        }
        if (r.timed_out) {
            if (!out.message.empty()) out.message += "\n";
            out.message += "[hook] timed out after " +
                           std::to_string(h.timeout_ms) + "ms";
        }
        // Only a blocking pre_tool hook can refuse; a post_tool hook runs after
        // the fact and has nothing to veto.
        if (event == "pre_tool" && h.blocking && r.exit_code != 0) {
            out.allowed = false;
            if (!out.message.empty()) out.message += "\n";
            out.message += "[hook] refused " + tool + " (exit " +
                           std::to_string(r.exit_code) + ")";
            return out;
        }
    }
    return out;
}

void install_hooks(ToolRegistry& registry, const std::vector<Hook>& hooks) {
    if (hooks.empty()) return;

    // Wrap each tool's handler. Read-only tools are left alone: firing a shell
    // command around every read_file would dominate the cost of the read.
    for (const std::string& name : registry.names()) {
        const Tool* orig = registry.find(name);
        if (!orig || orig->kind == ToolKind::Read) continue;

        Tool wrapped = *orig;
        ToolHandler inner = orig->handler;
        std::vector<Hook> captured = hooks;
        std::string tool_name = name;

        wrapped.handler = [inner, captured, tool_name](
                              const json& args, ToolContext& ctx) -> ToolResult {
            HookOutcome pre = run_hooks(captured, "pre_tool", tool_name, args,
                                        ctx.cwd, "", false);
            if (!pre.allowed)
                return ToolResult::err("blocked by a pre_tool hook.\n" + pre.message);

            ToolResult r = inner(args, ctx);

            HookOutcome post = run_hooks(captured, "post_tool", tool_name, args,
                                         ctx.cwd, r.content, r.is_error);

            std::string extra;
            if (!pre.message.empty()) extra += pre.message + "\n";
            if (!post.message.empty()) extra += post.message + "\n";
            if (!extra.empty()) r.content += "\n\n" + trim(extra);
            return r;
        };
        registry.add(std::move(wrapped));
    }
}

} // namespace ppcode::commands

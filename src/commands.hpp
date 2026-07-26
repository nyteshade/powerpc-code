// commands.hpp -- user-defined slash commands, and hooks around tool calls.
//
// Both exist so the tool can be shaped without recompiling it. A slash command
// is a markdown file whose body becomes a prompt; a hook is a shell command run
// before or after a tool, which can also refuse the call.
//
// The file format is the same YAML frontmatter used by job files and agent
// definitions, so there is one thing to learn rather than three.
#pragma once

#include "common.hpp"
#include "config.hpp"
#include "tools.hpp"

namespace ppcode::commands {

struct Command {
    std::string name;          // invoked as /name
    std::string description;   // shown by /help
    std::string body;          // prompt template
    std::string model;         // optional model override for this command
    std::vector<std::string> allow_tools;
    std::string source_path;

    // Substitute arguments into the body. $ARGUMENTS is the whole argument
    // string; $1..$9 are whitespace-separated words. A body with no placeholder
    // gets the arguments appended, so a command is useful without ceremony.
    std::string expand(const std::string& args) const;
};

// Directories searched, in order:
//   $PPCODE_COMMANDS_DIR
//   ~/.config/ppcode/commands
//   ./.ppcode/commands
std::vector<std::string> command_dirs();

std::vector<Command> load(std::vector<std::string>* warnings);
const Command* find(const std::vector<Command>& cmds, const std::string& name);

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

struct Hook {
    std::string event;         // "pre_tool" | "post_tool"
    std::string match;         // tool name, or empty for all; "*" also matches all
    std::string command;       // shell command
    bool blocking = false;     // pre_tool only: a non-zero exit refuses the call
    int timeout_ms = 30000;
};

// Parsed from config.json:
//   "hooks": [ {"event":"post_tool","match":"edit_file","command":"..."} ]
std::vector<Hook> load_hooks(const json& config, std::vector<std::string>* warnings);

struct HookOutcome {
    bool allowed = true;       // a blocking pre_tool hook can set this false
    std::string message;       // anything the hook printed, for the transcript
};

// Run every hook matching this event and tool. The tool's arguments are exposed
// to the command as environment variables so a hook can act on them:
//   PPCODE_TOOL, PPCODE_ARGS (JSON), PPCODE_CWD, and for post_tool
//   PPCODE_RESULT and PPCODE_IS_ERROR.
HookOutcome run_hooks(const std::vector<Hook>& hooks, const std::string& event,
                      const std::string& tool, const json& args,
                      const std::string& cwd, const std::string& result,
                      bool is_error);

// Wrap every non-read tool in the registry so the hooks fire around it.
void install_hooks(ToolRegistry& registry, const std::vector<Hook>& hooks);

} // namespace ppcode::commands

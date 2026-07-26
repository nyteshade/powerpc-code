// subagent.hpp -- spawning sub-conversations, and custom agent definitions.
//
// Two reasons this matters here specifically:
//
//   Context.  A subagent explores in its own window and returns only its
//   conclusion, so the parent's context is not filled with the fifty files the
//   answer required. On a machine where every round is slow, that is the
//   difference between finishing a task and running out of window.
//
//   Cost.  Bulk work does not need a frontier model. Pointing a fan-out of
//   searchers at a cheap model while the parent stays on an expensive one is
//   often an order-of-magnitude saving, which is why the model is selectable
//   per agent rather than fixed.
//
// Custom agents are markdown files with YAML frontmatter, the same shape as job
// files:
//
//   ---
//   name: doc-searcher
//   description: Finds API details in the local Apple documentation.
//   model: deepseek/deepseek-v4-pro
//   tools: [read_file, grep, glob, apple_docs]
//   ---
//   You are a documentation specialist. Answer with the exact declaration and
//   the file it came from. Do not speculate.
#pragma once

#include "common.hpp"
#include "config.hpp"
#include "openrouter.hpp"
#include "tools.hpp"

#include <mutex>

namespace ppcode::subagent {

struct Definition {
    std::string name;
    std::string description;     // shown to the model so it can choose
    std::string system_prompt;   // the markdown body
    std::string model;           // empty inherits the parent's model
    std::vector<std::string> tools;   // empty means every tool except spawning
    std::optional<int> max_turns;
    bool inherit_context = true; // include the machine/platform system context
    std::string source_path;
};

// Directories searched for agent definitions, in order:
//   $PPCODE_AGENTS_DIR
//   ~/.config/ppcode/agents
//   ./.ppcode/agents
//   ./agents
std::vector<std::string> definition_dirs();

// Load every *.md found, deduplicated by name with earlier directories winning.
std::vector<Definition> load_definitions(std::vector<std::string>* warnings);

// Everything a spawned agent needs from its parent.
struct Host {
    Client* client = nullptr;          // for its configuration, not reused directly
    const Config* config = nullptr;
    const ToolRegistry* parent_tools = nullptr;
    std::string cwd;

    // The parent's system context (machine probe, knowledge). Subagents inherit
    // it unless their definition opts out, so they know what platform they are
    // on without paying to rebuild it.
    std::string base_system;

    // Usage rolls up so the parent's spend cap counts subagent tokens.
    std::mutex* usage_mutex = nullptr;
    Usage* shared_usage = nullptr;

    // Approvals are serialised: with several agents running at once, two
    // simultaneous prompts would be unreadable.
    std::mutex* approve_mutex = nullptr;
    std::function<bool(const std::string&, ToolKind, const ToolPreview&)> approve;

    // Progress, so a long fan-out is not silent.
    std::function<void(const std::string&)> note;

    std::atomic<bool>* cancel = nullptr;

    int max_parallel = 4;              // network-bound, so more than cores is fine
};

struct Result {
    bool ok = false;
    std::string error;
    std::string text;
    Usage usage;
    int rounds = 0;
    int tool_calls = 0;
    std::string agent;
    std::string model;
};

// Run one subagent to completion.
Result run(const Definition& def, const std::string& prompt, const Host& host);

// Register the `task` and `task_batch` tools. `definitions` is captured.
void add_tools(ToolRegistry& registry, const Host& host,
               const std::vector<Definition>& definitions);

// The built-in general-purpose agent, used when no type is named.
Definition general_purpose();

} // namespace ppcode::subagent

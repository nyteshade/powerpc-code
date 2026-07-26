// sysprompt.hpp -- assemble the system message for a turn.
//
// The system message is where all the host-specific knowledge lands: what this
// machine is, what it can build, and what a model needs to know about writing
// code for a 2005 PowerPC Mac. How much of it we can afford depends entirely on
// the model, so this module takes the context window as an input and scales the
// content to fit rather than emitting a fixed blob.
#pragma once

#include "common.hpp"
#include "config.hpp"
#include "envinfo.hpp"

namespace ppcode::sysprompt {

// One knowledge document: platform notes, API availability, gotchas. Loaded
// from markdown files with optional YAML frontmatter so the corpus can grow
// without recompiling.
struct Doc {
    std::string id;            // filename stem
    std::string title;
    std::string body;          // markdown, frontmatter stripped
    int priority = 50;         // lower is included first
    int64_t min_context = 0;   // skip unless the window is at least this big
    std::vector<std::string> tags;
    std::string path;

    size_t est_tokens() const;
};

// Directories searched for knowledge documents, in order:
//   $PPCODE_KNOWLEDGE_DIR
//   ~/.config/ppcode/knowledge
//   <dir of the running binary>/../knowledge
//   ./knowledge
std::vector<std::string> knowledge_dirs();

// Per-project instructions, the equivalent of a CLAUDE.md. Searched from `cwd`
// upwards to the repository root (or the filesystem root), so running ppcode in
// a subdirectory still finds the project's file. Accepted names, in order of
// preference: .ppcode.md, ppcode.md, AGENTS.md, CLAUDE.md.
//
// Returns the concatenated contents, outermost file first, so a repository-wide
// file is followed by a more specific one that can override it.
struct ProjectDoc {
    std::string path;
    std::string body;
};
std::vector<ProjectDoc> load_project_docs(const std::string& cwd);

// Load every *.md found, deduplicated by id with earlier directories winning,
// sorted by priority. Never throws; unreadable files are skipped.
std::vector<Doc> load_docs(std::vector<std::string>* warnings = nullptr);

struct Inputs {
    const Config* cfg = nullptr;
    const envinfo::Probe* probe = nullptr;
    std::string cwd;

    // Model shape. context_tokens drives every budget decision.
    int64_t context_tokens = 0;
    bool model_supports_images = false;
    bool model_supports_tools = true;
    std::string model_id;

    // Names of the tools actually available this run, so the prompt can point
    // at them specifically rather than describing tools that are not loaded.
    std::vector<std::string> tool_names;

    // Explicit override from a job file or the CLI; unset means choose.
    std::optional<envinfo::Detail> env_detail;

    // Set false to leave the knowledge corpus out entirely.
    bool include_knowledge = true;
    // Set false to ignore any .ppcode.md in the project.
    bool include_project_docs = true;

    // Fraction of the context window the whole system message may occupy.
    double budget_fraction = 0.10;
};

struct Result {
    std::string text;
    envinfo::Detail env_detail = envinfo::Detail::None;
    size_t est_tokens = 0;
    std::vector<std::string> included_docs;
    std::vector<std::string> skipped_docs;   // dropped for budget or context
    std::vector<std::string> project_docs;   // paths of any .ppcode.md found
};

Result build(const Inputs& in);

} // namespace ppcode::sysprompt

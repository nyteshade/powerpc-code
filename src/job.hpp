// job.hpp -- markdown job files: YAML frontmatter plus a prose task description.
//
// A job file is a self-contained, checkable-in description of a piece of work:
// which model to use and how to route it, what the model may touch, what to
// attach, and what to do. The body is ordinary markdown, so a job file doubles
// as documentation of the task.
//
//   ---
//   name: build-sample
//   model: z-ai/glm-5.2
//   provider:
//     sort: throughput
//     order: [together, fireworks]
//     allow_fallbacks: true
//   reasoning:
//     effort: high
//   max_turns: 30
//   tools:
//     allow: [write_file, edit_file, bash, run_background]
//   attachments:
//     - path: docs/mockup.png
//   environment:
//     detail: full
//   ---
//
//   Build the Sample Xcode project and fix any warnings.
#pragma once

#include "common.hpp"
#include "config.hpp"
#include "openrouter.hpp"

namespace ppcode::job {

// One thing to hand to the model alongside the prompt.
struct Attachment {
    std::string path;      // local file, or a URL for images
    std::string kind;      // "auto" | "image" | "text"
    std::string detail;    // images only: "auto" | "low" | "high"
    std::string alt;       // optional label shown in the transcript
};

struct Spec {
    // Identity, for logs and for --list.
    std::string name;
    std::string description;

    // Model and routing. Empty fields mean "leave the config's value alone".
    std::string model;
    std::vector<std::string> model_fallbacks;
    json provider;                     // OpenRouter provider preferences
    json reasoning;                    // effort / max_tokens / exclude

    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<int> max_tokens;
    std::optional<int> max_turns;
    std::optional<int> seed;

    std::optional<bool> web_search;
    std::optional<int> web_max_results;
    std::optional<double> max_cost;
    std::string cache_mode;

    // Execution
    std::string cwd;
    std::optional<bool> yolo;
    std::vector<std::string> allow_tools;
    std::vector<std::string> deny_tools;

    // System context
    std::string env_detail;            // none|minimal|brief|standard|full
    std::optional<bool> knowledge;
    std::string system;                // replaces the built-in instructions
    std::string system_append;         // added after everything else

    // Output
    std::string output;                // text|json|stream-json
    std::string save;
    std::string resume;

    std::vector<Attachment> attachments;

    // The markdown body: the actual task.
    std::string prompt;

    std::string source_path;

    bool valid(std::string* error) const;
};

// Parse a job file. Frontmatter problems are fatal (`error`); unknown or
// unusable keys are reported through `warnings` and ignored, so a typo does not
// silently change behaviour.
bool parse_text(const std::string& text, Spec* out,
                std::vector<std::string>* warnings, std::string* error);

bool parse_file(const std::string& path, Spec* out,
                std::vector<std::string>* warnings, std::string* error);

// Fold the spec's model and routing settings into a Config.
void apply_to_config(const Spec& spec, Config* cfg);

// Build the user message for this job, attaching files as content parts.
// `model_supports_images` decides whether images are embedded or described;
// either way the caller gets a usable message and any problems in `warnings`.
Message build_user_message(const Spec& spec, bool model_supports_images,
                           std::vector<std::string>* warnings);

} // namespace ppcode::job

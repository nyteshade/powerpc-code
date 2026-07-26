// headless.hpp -- non-interactive execution, for scripting ppcode.
#pragma once

#include "agent.hpp"
#include "common.hpp"
#include "config.hpp"

namespace ppcode {

enum class OutputFormat {
    Text,        // assistant text on stdout, activity on stderr
    Json,        // one JSON object at the end
    StreamJson,  // JSONL: one event per line, as it happens
};

struct HeadlessOptions {
    std::string prompt;             // if empty, read stdin (unless `message` is set)

    // A pre-built first turn, used for job files with attachments. When set,
    // `prompt` is ignored and stdin is not read.
    std::optional<Message> message;
    OutputFormat format = OutputFormat::Text;
    bool yolo = false;
    std::vector<std::string> allow_tools;   // extra tools allowed past the gate
    std::vector<std::string> deny_tools;
    bool quiet = false;             // suppress activity on stderr
    std::string resume_path;        // load a saved session first
    std::string save_path;          // write the session out afterwards
};

// Returns a process exit code: 0 success, 1 model/tool error, 2 usage error.
int run_headless(Agent& agent, const Config& cfg, const HeadlessOptions& opt);

} // namespace ppcode

// config.hpp -- persisted settings plus the API key sourced from the environment.
#pragma once

#include "common.hpp"

namespace ppcode {

// One configured MCP server. `transport` decides which fields matter:
//   "stdio" -> command + args + env   (spawned locally)
//   "http"  -> url + headers          (Streamable HTTP / SSE, typically on the LAN)
struct McpServerConfig {
    std::string name;
    std::string transport = "stdio";
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string url;
    std::map<std::string, std::string> headers;
    bool enabled = true;
};

struct Config {
    std::string model = "anthropic/claude-sonnet-5";
    std::string base_url = "https://openrouter.ai/api/v1";
    std::string api_key;              // never persisted to disk
    std::string system_prompt;        // empty => built-in default
    double temperature = 1.0;
    int max_tokens = 8192;
    int max_turns = 40;               // tool-call rounds before forcing a stop

    // OpenRouter attribution headers (optional but encouraged by their docs).
    std::string referer = "https://github.com/nyteshade/ppcode";
    std::string title = "ppcode";

    // UI
    bool unicode = false;             // LC_CTYPE is "C" on this box; ASCII by default
    bool color = true;

    // Tool policy
    bool auto_approve_reads = true;   // read_file/list_dir/glob/grep need no prompt
    bool yolo = false;                // approve every tool without asking

    std::vector<McpServerConfig> mcp_servers;

    std::string config_path;          // where this was loaded from

    // Resolve config from (in increasing precedence): defaults, config file,
    // environment. Never throws; `warning` receives non-fatal problems.
    static Config load(const std::string& explicit_path,
                       std::vector<std::string>* warnings);

    // Write the non-secret parts back out.
    bool save(std::string* error) const;

    // Default location: $XDG_CONFIG_HOME/ppcode/config.json or ~/.config/ppcode/config.json
    static std::string default_path();

    // The API key is read from OPENROUTER_AI_API_KEY, falling back to
    // OPENROUTER_API_KEY. Returns empty if neither is set.
    static std::string key_from_env();

    std::string default_system_prompt() const;
    std::string effective_system_prompt() const;
};

} // namespace ppcode

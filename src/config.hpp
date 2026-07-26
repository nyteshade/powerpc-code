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
    double top_p = -1.0;              // negative means "do not send"
    int seed = -1;                    // negative means "do not send"

    // OpenRouter request fields kept as raw JSON so that new options in their
    // API work without a code change here. `provider` carries routing
    // preferences (sort, order, allow_fallbacks, only/ignore, data_collection,
    // quantizations); `reasoning` carries effort or a thinking-token budget.
    json provider;
    json reasoning;

    // Ordered fallback models for OpenRouter's "models" field.
    std::vector<std::string> model_fallbacks;

    // OpenRouter's own web plugin. Needs no extra credentials, which makes it
    // the most reliable search option on a machine like this.
    bool web_search = false;
    int web_max_results = 5;

    // Prompt caching. The system message here is thousands of tokens of machine
    // and platform context that is identical on every round of a turn, so
    // caching it is the single biggest cost lever available.
    //   "auto" -- enable for providers that need an explicit breakpoint
    //   "on"   -- always send cache_control
    //   "off"  -- never
    std::string cache_mode = "auto";

    // Transient failures are common enough on a long agentic run that giving up
    // on the first 429 makes unattended jobs unreliable.
    int max_retries = 4;

    // Stop the run once this much has been spent, in USD. Zero disables it.
    double max_cost = 0.0;
    // Warn once cumulative spend passes this fraction of max_cost.
    double cost_warn_fraction = 0.75;

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

// main.cpp -- argument parsing and dispatch.
#include "agent.hpp"
#include "common.hpp"
#include "config.hpp"
#include "headless.hpp"
#include "http.hpp"
#include "mcp.hpp"
#include "openrouter.hpp"
#include "selftest.hpp"
#include "tools.hpp"
#include "ui.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace ppcode;

namespace {

const char* kUsage =
    "ppcode -- a terminal coding assistant for OpenRouter, native on PowerPC Leopard.\n"
    "\n"
    "USAGE\n"
    "  ppcode                          start the interactive TUI\n"
    "  ppcode -p \"prompt\"              run one prompt and exit\n"
    "  echo \"prompt\" | ppcode -p       read the prompt from stdin\n"
    "\n"
    "OPTIONS\n"
    "  -p, --print [PROMPT]     non-interactive; prompt may also come from stdin\n"
    "      --output FORMAT      text (default), json, or stream-json\n"
    "  -m, --model ID           model to use, e.g. anthropic/claude-sonnet-4.5\n"
    "  -C, --cwd DIR            working directory for tools\n"
    "  -c, --config PATH        config file (default ~/.config/ppcode/config.json)\n"
    "      --yolo               allow every tool without asking\n"
    "      --allow-tool NAME    permit one tool in headless mode (repeatable)\n"
    "      --deny-tool NAME     forbid one tool (repeatable, wins over --yolo)\n"
    "      --max-turns N        cap tool rounds per prompt (default 40)\n"
    "  -q, --quiet              suppress progress output on stderr\n"
    "      --resume PATH        load a saved session first\n"
    "      --save PATH          write the session out when done\n"
    "      --list-models [SUB]  list available models, optionally filtered\n"
    "      --write-config       write a default config file and exit\n"
    "      --selftest [--net]   run internal checks\n"
    "      --log PATH           append a debug log\n"
    "      --version            print the version and exit\n"
    "  -h, --help               this text\n"
    "\n"
    "ENVIRONMENT\n"
    "  OPENROUTER_AI_API_KEY    required (OPENROUTER_API_KEY also accepted)\n"
    "  PPCODE_MODEL             overrides the configured model\n"
    "\n"
    "EXAMPLES\n"
    "  ppcode -p \"what does src/http.cpp do?\"\n"
    "  ppcode -p \"add a --version flag\" --allow-tool edit_file --allow-tool write_file\n"
    "  ppcode -p \"summarise the build\" --output json | python3 -c 'import json,sys;"
    "print(json.load(sys.stdin)[\"text\"])'\n"
    "  ppcode -p \"fix the warning\" --output stream-json --yolo\n";

// True if `arg` needs a value and one is available.
bool take_value(int argc, char** argv, int* i, const char* name, std::string* out) {
    if (*i + 1 >= argc) {
        std::fprintf(stderr, "ppcode: %s requires a value\n", name);
        return false;
    }
    *out = argv[++(*i)];
    return true;
}

int cmd_list_models(const Config& cfg, const std::string& filter) {
    Client client(cfg);
    std::string err;
    std::vector<ModelInfo> models = client.list_models(&err);
    if (models.empty()) {
        std::fprintf(stderr, "ppcode: could not list models: %s\n",
                     err.empty() ? "no results" : err.c_str());
        return 1;
    }
    std::string needle = to_lower(filter);
    int shown = 0;
    for (const ModelInfo& m : models) {
        if (!needle.empty() && to_lower(m.id).find(needle) == std::string::npos) continue;
        // Pricing is per token; per-million is the readable unit.
        std::printf("%-52s %9lld ctx  $%7.2f/$%7.2f per Mtok%s\n", m.id.c_str(),
                    static_cast<long long>(m.context_length),
                    m.prompt_cost * 1e6, m.completion_cost * 1e6,
                    m.supports_tools ? "  [tools]" : "");
        shown++;
    }
    if (shown == 0) {
        std::fprintf(stderr, "no models matched \"%s\"\n", filter.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path, model, cwd, log_path;
    std::string output_format = "text";
    std::string list_filter;
    bool want_print = false, want_selftest = false, want_net = false;
    bool want_list = false, want_write_config = false, want_help = false;
    bool want_version = false;
    bool yolo = false, quiet = false;
    int max_turns = -1;
    std::string prompt, resume_path, save_path;
    std::vector<std::string> allow_tools, deny_tools;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* n, std::string* out) { return take_value(argc, argv, &i, n, out); };

        if (a == "-h" || a == "--help")            want_help = true;
        else if (a == "--version")                 want_version = true;
        else if (a == "--selftest")                want_selftest = true;
        else if (a == "--net")                     want_net = true;
        else if (a == "--yolo")                    yolo = true;
        else if (a == "-q" || a == "--quiet")      quiet = true;
        else if (a == "--write-config")            want_write_config = true;
        else if (a == "-p" || a == "--print") {
            want_print = true;
            // The prompt is optional here; without it we read stdin.
            if (i + 1 < argc && argv[i + 1][0] != '-') prompt = argv[++i];
        }
        else if (a == "--list-models") {
            want_list = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') list_filter = argv[++i];
        }
        else if (a == "-m" || a == "--model")   { if (!need("--model", &model)) return 2; }
        else if (a == "-C" || a == "--cwd")     { if (!need("--cwd", &cwd)) return 2; }
        else if (a == "-c" || a == "--config")  { if (!need("--config", &config_path)) return 2; }
        else if (a == "--output")               { if (!need("--output", &output_format)) return 2; }
        else if (a == "--resume")               { if (!need("--resume", &resume_path)) return 2; }
        else if (a == "--save")                 { if (!need("--save", &save_path)) return 2; }
        else if (a == "--log")                  { if (!need("--log", &log_path)) return 2; }
        else if (a == "--max-turns") {
            std::string v;
            if (!need("--max-turns", &v)) return 2;
            max_turns = std::atoi(v.c_str());
        }
        else if (a == "--allow-tool") {
            std::string v;
            if (!need("--allow-tool", &v)) return 2;
            allow_tools.push_back(v);
        }
        else if (a == "--deny-tool") {
            std::string v;
            if (!need("--deny-tool", &v)) return 2;
            deny_tools.push_back(v);
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "ppcode: unknown option %s (try --help)\n", a.c_str());
            return 2;
        }
        else {
            // A bare argument is treated as the prompt, so `ppcode -p "x"` and
            // `ppcode "x"` both work.
            if (prompt.empty()) { prompt = a; want_print = true; }
        }
    }

    if (want_help) {
        std::fputs(kUsage, stdout);
        return 0;
    }

    if (want_version) {
        std::printf("ppcode 0.1.0 (powerpc-apple-darwin9) %s\n",
                    http::version_string().c_str());
        return 0;
    }

    if (!log_path.empty()) log_init(log_path);
    else if (const char* e = std::getenv("PPCODE_LOG"); e && *e) log_init(e);

    http::global_init();
    struct CurlGuard { ~CurlGuard() { http::global_cleanup(); } } curl_guard;

    if (want_selftest) return run_selftest(want_net) == 0 ? 0 : 1;

    std::vector<std::string> warnings;
    Config cfg = Config::load(config_path, &warnings);
    for (const std::string& w : warnings)
        std::fprintf(stderr, "ppcode: %s\n", w.c_str());

    if (!model.empty())   cfg.model = model;
    if (max_turns > 0)    cfg.max_turns = max_turns;
    if (yolo)             cfg.yolo = true;

    if (want_write_config) {
        std::string err;
        if (!cfg.save(&err)) {
            std::fprintf(stderr, "ppcode: %s\n", err.c_str());
            return 1;
        }
        std::printf("wrote %s\n", cfg.config_path.c_str());
        return 0;
    }

    if (want_list) return cmd_list_models(cfg, list_filter);

    if (cfg.api_key.empty()) {
        std::fprintf(stderr,
                     "ppcode: no API key. Set OPENROUTER_AI_API_KEY in your environment.\n");
        return 2;
    }

    Client client(cfg);
    ToolRegistry tools;
    tools.add_builtins();

    // MCP servers contribute additional tools. A server that fails to start is
    // reported and skipped -- it must not prevent ppcode from running.
    mcp::Manager mcp_manager;
    if (!cfg.mcp_servers.empty()) {
        mcp_manager.connect_all(cfg.mcp_servers, tools, [&](const std::string& m) {
            if (!quiet) std::fprintf(stderr, "%s\n", m.c_str());
        });
    }
    struct McpGuard {
        mcp::Manager& m;
        ~McpGuard() { m.disconnect_all(); }
    } mcp_guard{mcp_manager};

    Agent agent(client, tools, cfg);
    if (!cwd.empty()) {
        std::string resolved = expand_user(cwd);
        if (chdir(resolved.c_str()) != 0) {
            std::fprintf(stderr, "ppcode: cannot enter %s\n", resolved.c_str());
            return 2;
        }
        char buf[4096];
        agent.set_cwd(getcwd(buf, sizeof(buf)) ? buf : resolved);
    }

    if (want_print) {
        HeadlessOptions opt;
        opt.prompt = prompt;
        opt.yolo = cfg.yolo;
        opt.allow_tools = allow_tools;
        opt.deny_tools = deny_tools;
        opt.quiet = quiet;
        opt.resume_path = resume_path;
        opt.save_path = save_path;

        if (output_format == "text")             opt.format = OutputFormat::Text;
        else if (output_format == "json")        opt.format = OutputFormat::Json;
        else if (output_format == "stream-json") opt.format = OutputFormat::StreamJson;
        else {
            std::fprintf(stderr,
                         "ppcode: --output must be text, json, or stream-json\n");
            return 2;
        }
        return run_headless(agent, cfg, opt);
    }

    if (!isatty(STDIN_FILENO)) {
        std::fprintf(stderr,
                     "ppcode: stdin is not a terminal. Use -p to run non-interactively.\n");
        return 2;
    }

    return run_tui(agent, client, tools, cfg, &mcp_manager);
}

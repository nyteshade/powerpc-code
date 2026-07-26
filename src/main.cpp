// main.cpp -- argument parsing and dispatch.
#include "agent.hpp"
#include "appledocs.hpp"
#include "builderr.hpp"
#include "common.hpp"
#include "config.hpp"
#include "envinfo.hpp"
#include "headless.hpp"
#include "http.hpp"
#include "job.hpp"
#include "macgui.hpp"
#include "jobs.hpp"
#include "mcp.hpp"
#include "openrouter.hpp"
#include "session.hpp"
#include "subagent.hpp"
#include "sysprompt.hpp"
#include "webtools.hpp"
#include "xcodeproj.hpp"
#include "selftest.hpp"
#include "tools.hpp"
#include "ui.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unistd.h>

using namespace ppcode;

namespace {

// Shared across subagents: usage rolls up so the parent's spend cap counts
// their tokens, and approvals are serialised so a fan-out cannot interleave
// two prompts on the same terminal.
std::mutex g_subagent_usage_mutex;
Usage g_subagent_usage;
std::mutex g_subagent_approve_mutex;

const char* kUsage =
    "ppcode -- a terminal coding assistant for OpenRouter, native on PowerPC Leopard.\n"
    "\n"
    "USAGE\n"
    "  ppcode                          start the interactive TUI\n"
    "  ppcode -p \"prompt\"              run one prompt and exit\n"
    "  echo \"prompt\" | ppcode -p       read the prompt from stdin\n"
    "  ppcode -j task.md               run a markdown job file\n"
    "\n"
    "OPTIONS\n"
    "  -p, --print [PROMPT]     non-interactive; prompt may also come from stdin\n"
    "  -j, --job FILE           run a job file: YAML frontmatter + markdown body\n"
    "      --attach PATH        attach a file or image (repeatable)\n"
    "      --dry-run            with --job, show the resolved settings and exit\n"
    "      --output FORMAT      text (default), json, or stream-json\n"
    "  -m, --model ID           model to use, e.g. anthropic/claude-sonnet-4.5\n"
    "  -C, --cwd DIR            working directory for tools\n"
    "  -c, --config PATH        config file (default ~/.config/ppcode/config.json)\n"
    "      --yolo               allow every tool without asking\n"
    "      --allow-tool NAME    permit one tool in headless mode (repeatable)\n"
    "      --deny-tool NAME     forbid one tool (repeatable, wins over --yolo)\n"
    "      --max-turns N        cap tool rounds per prompt (default 40)\n"
    "      --max-cost USD       stop once this much has been spent this session\n"
    "      --no-cache           disable prompt caching (on by default where supported)\n"
    "  -q, --quiet              suppress progress output on stderr\n"
    "  -r, --continue           resume the most recent session in this directory\n"
    "      --resume [ID]        resume a session by id, or the most recent\n"
    "      --sessions           list saved sessions and exit\n"
    "      --no-save            do not persist this session\n"
    "      --save PATH          write the session out when done\n"
    "      --env-detail LEVEL   machine context: none|minimal|brief|standard|full\n"
    "      --refresh-env        re-probe the machine instead of using the cache\n"
    "      --no-knowledge       omit the platform knowledge documents\n"
    "      --no-project-docs    ignore any .ppcode.md in the project\n"
    "      --show-context       report how the system message was assembled\n"
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
    bool refresh_env = false, no_knowledge = false, show_context = false;
    bool no_project_docs = false;
    std::string env_detail_opt, job_path;
    bool dry_run = false;
    std::vector<std::string> attach_paths;
    int max_turns = -1;
    double max_cost = -1.0;
    bool cache_off = false;
    std::string prompt, resume_path, save_path, resume_id;
    bool want_continue = false, want_sessions = false, no_save = false;
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
        else if (a == "--refresh-env")             refresh_env = true;
        else if (a == "--no-knowledge")            no_knowledge = true;
        else if (a == "--no-project-docs")         no_project_docs = true;
        else if (a == "--show-context")            show_context = true;
        else if (a == "--dry-run")                 dry_run = true;
        else if (a == "--env-detail") { if (!need("--env-detail", &env_detail_opt)) return 2; }
        else if (a == "-j" || a == "--job") { if (!need("--job", &job_path)) return 2; }
        else if (a == "--attach") {
            std::string v;
            if (!need("--attach", &v)) return 2;
            attach_paths.push_back(v);
        }
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
        else if (a == "--resume") {
            // --resume alone opens the most recent; --resume ID or PATH targets one.
            if (i + 1 < argc && argv[i + 1][0] != '-') resume_id = argv[++i];
            else want_continue = true;
        }
        else if (a == "-r" || a == "--continue")   want_continue = true;
        else if (a == "--sessions")                want_sessions = true;
        else if (a == "--no-save")                 no_save = true;
        else if (a == "--save")                 { if (!need("--save", &save_path)) return 2; }
        else if (a == "--log")                  { if (!need("--log", &log_path)) return 2; }
        else if (a == "--max-turns") {
            std::string v;
            if (!need("--max-turns", &v)) return 2;
            max_turns = std::atoi(v.c_str());
        }
        else if (a == "--max-cost") {
            std::string v;
            if (!need("--max-cost", &v)) return 2;
            max_cost = std::atof(v.c_str());
        }
        else if (a == "--no-cache")                cache_off = true;
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

    // A job file is read before the CLI overrides so that explicit flags still
    // win over what the file says.
    job::Spec jobspec;
    bool have_job = false;
    if (!job_path.empty()) {
        std::vector<std::string> jwarn;
        std::string jerr;
        if (!job::parse_file(job_path, &jobspec, &jwarn, &jerr)) {
            std::fprintf(stderr, "ppcode: %s\n", jerr.c_str());
            return 2;
        }
        for (const std::string& w : jwarn)
            std::fprintf(stderr, "ppcode: job: %s\n", w.c_str());
        job::apply_to_config(jobspec, &cfg);
        have_job = true;

        for (const std::string& t : jobspec.allow_tools) allow_tools.push_back(t);
        for (const std::string& t : jobspec.deny_tools)  deny_tools.push_back(t);
        if (!jobspec.env_detail.empty() && env_detail_opt.empty())
            env_detail_opt = jobspec.env_detail;
        if (jobspec.knowledge && !*jobspec.knowledge) no_knowledge = true;
        if (!jobspec.output.empty() && output_format == "text")
            output_format = jobspec.output;
        if (!jobspec.save.empty() && save_path.empty())     save_path = jobspec.save;
        if (!jobspec.resume.empty() && resume_path.empty()) resume_path = jobspec.resume;
        if (!jobspec.cwd.empty() && cwd.empty())            cwd = jobspec.cwd;
        want_print = true;
    }

    if (!model.empty())   cfg.model = model;
    if (max_turns > 0)    cfg.max_turns = max_turns;
    if (max_cost >= 0)    cfg.max_cost = max_cost;
    if (cache_off)        cfg.cache_mode = "off";
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

    if (want_sessions) {
        std::vector<session::Meta> all = session::list(40);
        if (all.empty()) {
            std::printf("no saved sessions\n");
            return 0;
        }
        for (const session::Meta& m : all)
            std::printf("%-24s %-10s %3d msg  $%.4f  %s\n    %s\n", m.id.c_str(),
                        m.age().c_str(), m.message_count, m.cost,
                        elide(m.cwd, 46).c_str(), elide(m.title, 76).c_str());
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
    TodoStore todos;
    static JobManager jobs;      // static: background jobs outlive this scope

    tools.add_builtins();
    tools.add_extra_builtins(&todos);
    add_job_tools(tools, jobs);
    web::add_tools(tools, web::SearchConfig::from_env());
    xcode::add_tools(tools);
    if (appledocs::available()) appledocs::add_tools(tools);
    builderr::add_tools(tools);

    // Screenshot tooling is only worth advertising if there is a screen to
    // capture. Whether the model can actually see the result changes the tool's
    // description, so look the model up first.
    if (macgui::screencapture_available()) {
        ModelCatalog vision_cat;
        vision_cat.load(client, nullptr);
        const ModelInfo* vm = vision_cat.find(cfg.model);
        macgui::add_tools(tools, vm ? vm->supports_images : false);
    }

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

    // Assemble the system message: base instructions, this machine's actual
    // capabilities, and as much platform knowledge as the model's context window
    // can carry. Probing shells out to sysctl/port and is cached on disk.
    {
        envinfo::Probe probe = envinfo::probe(refresh_env);

        ModelCatalog catalog;
        std::string cat_err;
        catalog.load(client, &cat_err);
        const ModelInfo* mi = catalog.find(cfg.model);

        sysprompt::Inputs si;
        si.cfg = &cfg;
        si.probe = &probe;
        si.cwd = agent.cwd();
        si.model_id = cfg.model;
        si.context_tokens = mi ? mi->context_length : ModelCatalog::kUnknownContext;
        si.model_supports_images = mi ? mi->supports_images : false;
        si.model_supports_tools = mi ? mi->supports_tools : true;
        si.tool_names = tools.names();
        si.include_knowledge = !no_knowledge;
        si.include_project_docs = !no_project_docs;
        if (!env_detail_opt.empty()) {
            bool ok = false;
            envinfo::Detail d = envinfo::detail_from_string(env_detail_opt, &ok);
            if (!ok) {
                std::fprintf(stderr,
                             "ppcode: --env-detail must be none, minimal, brief, "
                             "standard, or full\n");
                return 2;
            }
            si.env_detail = d;
        }

        sysprompt::Result sp = sysprompt::build(si);
        agent.set_system_prompt(sp.text);

        // Compaction needs to know the window; without it a long session simply
        // fails once it overflows.
        agent.set_context_limit(si.context_tokens);

        // Session persistence. Resolve what to resume, load it, then point the
        // agent at a file it writes after every completed turn.
        std::string load_path;
        if (!resume_id.empty()) {
            // Accept an id or a path.
            load_path = (resume_id.find('/') != std::string::npos)
                            ? expand_user(resume_id)
                            : session::path_for(resume_id);
        } else if (want_continue) {
            session::Meta m;
            if (session::most_recent(agent.cwd(), &m) ||
                session::most_recent("", &m)) {
                load_path = m.path;
                if (!quiet)
                    std::fprintf(stderr, "resuming %s (%s, %d messages): %s\n",
                                 m.id.c_str(), m.age().c_str(), m.message_count,
                                 elide(m.title, 60).c_str());
            } else if (!quiet) {
                std::fprintf(stderr, "ppcode: no previous session to resume\n");
            }
        } else if (!resume_path.empty()) {
            load_path = expand_user(resume_path);
        }

        if (!load_path.empty()) {
            std::string text, lerr;
            if (!read_file_text(load_path, &text, &lerr)) {
                std::fprintf(stderr, "ppcode: cannot resume: %s\n", lerr.c_str());
                return 2;
            }
            try {
                if (!agent.from_json(json::parse(text), &lerr)) {
                    std::fprintf(stderr, "ppcode: cannot resume: %s\n", lerr.c_str());
                    return 2;
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "ppcode: cannot resume: %s\n", e.what());
                return 2;
            }
            // The restored system prompt describes whatever machine and model
            // that session used; rebuild it for this run.
            agent.set_system_prompt(sp.text);
        }

        if (!no_save) {
            std::string sid = load_path.empty() ? session::new_id()
                                                : std::filesystem::path(load_path).stem().string();
            agent.set_session_path(load_path.empty() ? session::path_for(sid)
                                                     : load_path);
        }
        // Keep the directory from growing without bound.
        session::prune(30, 50);

        // Subagents inherit the platform context the parent just assembled, so
        // they know what machine they are on without rebuilding it. Registered
        // last so their tool roster can include everything above.
        {
            std::vector<std::string> awarn;
            std::vector<subagent::Definition> defs = subagent::load_definitions(&awarn);
            for (const std::string& w : awarn)
                std::fprintf(stderr, "ppcode: %s\n", w.c_str());

            static subagent::Host host;
            host.client = &client;
            host.config = &cfg;
            host.parent_tools = &tools;
            host.cwd = agent.cwd();
            host.base_system = sp.text;
            host.usage_mutex = &g_subagent_usage_mutex;
            host.shared_usage = &g_subagent_usage;
            host.approve_mutex = &g_subagent_approve_mutex;
            subagent::add_tools(tools, host, defs);

            if (show_context && !defs.empty())
                std::fprintf(stderr, "agents: %zu custom + general\n", defs.size());
        }

        // --show-context is an explicit request for diagnostics, so -q does not
        // suppress it.
        if (show_context) {
            std::fprintf(stderr,
                         "context: %zu est. tokens, env=%s, knowledge=[%s]%s\n",
                         sp.est_tokens,
                         envinfo::detail_to_string(sp.env_detail).c_str(),
                         join(sp.included_docs, " ").c_str(),
                         sp.skipped_docs.empty()
                             ? ""
                             : (" skipped=[" + join(sp.skipped_docs, " ") + "]").c_str());
            if (!sp.project_docs.empty())
                std::fprintf(stderr, "project docs: %s\n",
                             join(sp.project_docs, ", ").c_str());
        }
    }

    if (want_print) {
        HeadlessOptions opt;
        opt.prompt = prompt;
        opt.yolo = cfg.yolo;

        // Attachments come from the job file and from --attach; both need the
        // model's vision capability to decide how to carry them.
        if (have_job || !attach_paths.empty()) {
            job::Spec effective = jobspec;
            if (!have_job) effective.prompt = prompt;
            if (effective.cwd.empty()) effective.cwd = agent.cwd();
            for (const std::string& p : attach_paths)
                effective.attachments.push_back({p, "auto", "auto", ""});

            ModelCatalog cat2;
            cat2.load(client, nullptr);
            const ModelInfo* mi2 = cat2.find(cfg.model);
            bool vision = mi2 ? mi2->supports_images : false;

            if (!effective.attachments.empty() && !vision && !quiet)
                std::fprintf(stderr,
                             "ppcode: %s cannot see images; image attachments will be "
                             "described rather than shown\n", cfg.model.c_str());

            std::vector<std::string> awarn;
            Message um = job::build_user_message(effective, vision, &awarn);
            for (const std::string& w : awarn)
                std::fprintf(stderr, "ppcode: %s\n", w.c_str());
            opt.message = um;
        }

        if (dry_run) {
            std::printf("model:      %s\n", cfg.model.c_str());
            if (!cfg.model_fallbacks.empty())
                std::printf("fallbacks:  %s\n", join(cfg.model_fallbacks, ", ").c_str());
            if (cfg.provider.is_object() && !cfg.provider.empty())
                std::printf("provider:   %s\n", cfg.provider.dump().c_str());
            if (cfg.reasoning.is_object() && !cfg.reasoning.empty())
                std::printf("reasoning:  %s\n", cfg.reasoning.dump().c_str());
            std::printf("max_turns:  %d\nmax_tokens: %d\ntemperature: %.2f\n",
                        cfg.max_turns, cfg.max_tokens, cfg.temperature);
            std::printf("web_search: %s\n", cfg.web_search ? "on" : "off");
            std::printf("yolo:       %s\n", cfg.yolo ? "yes" : "no");
            if (!allow_tools.empty())
                std::printf("allow:      %s\n", join(allow_tools, ", ").c_str());
            if (!deny_tools.empty())
                std::printf("deny:       %s\n", join(deny_tools, ", ").c_str());
            std::printf("cwd:        %s\n", agent.cwd().c_str());
            std::printf("output:     %s\n", output_format.c_str());
            std::printf("tools:      %zu registered\n", tools.size());
            if (have_job) {
                std::printf("job:        %s (%s)\n", jobspec.name.c_str(),
                            jobspec.source_path.c_str());
                if (!jobspec.attachments.empty())
                    std::printf("attachments: %zu\n", jobspec.attachments.size());
                std::printf("\n--- prompt ---\n%s\n", jobspec.prompt.c_str());
            }
            return 0;
        }
        opt.allow_tools = allow_tools;
        opt.deny_tools = deny_tools;
        opt.quiet = quiet;
        opt.resume_path.clear();   // resume is resolved above
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

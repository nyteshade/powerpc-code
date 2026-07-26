#include "subagent.hpp"

#include "agent.hpp"
#include "yaml.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace ppcode::subagent {

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

std::vector<std::string> definition_dirs() {
    std::vector<std::string> dirs;
    if (const char* e = std::getenv("PPCODE_AGENTS_DIR"); e && *e) dirs.push_back(e);
    if (const char* h = std::getenv("HOME"); h && *h)
        dirs.push_back(std::string(h) + "/.config/ppcode/agents");
    dirs.push_back(".ppcode/agents");
    dirs.push_back("agents");
    return dirs;
}

std::vector<Definition> load_definitions(std::vector<std::string>* warnings) {
    std::vector<Definition> out;
    std::vector<std::string> seen;

    for (const std::string& dir : definition_dirs()) {
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
                if (warnings) warnings->push_back("agent: " + err);
                continue;
            }

            std::string front, body, ferr;
            if (!yaml::split_frontmatter(text, &front, &body, &ferr)) {
                if (warnings) warnings->push_back("agent " + f.string() + ": " + ferr);
                continue;
            }

            Definition d;
            d.source_path = f.string();
            d.name = f.stem().string();
            d.system_prompt = trim(body);

            if (!trim(front).empty()) {
                json meta;
                std::string yerr;
                if (!yaml::parse(front, &meta, &yerr) || !meta.is_object()) {
                    if (warnings)
                        warnings->push_back("agent " + d.name + " frontmatter: " + yerr);
                } else {
                    d.name = jstr(meta, "name", d.name);
                    d.description = jstr(meta, "description");
                    d.model = jstr(meta, "model");
                    d.inherit_context = jbool(meta, "inherit_context", true);
                    if (const json* mt = jptr(meta, "max_turns"); mt && mt->is_number())
                        d.max_turns = static_cast<int>(mt->get<int64_t>());
                    if (const json* t = jptr(meta, "tools")) {
                        if (t->is_string()) d.tools.push_back(t->get<std::string>());
                        else if (t->is_array())
                            for (const json& s : *t)
                                if (s.is_string()) d.tools.push_back(s.get<std::string>());
                    }
                }
            }

            if (d.system_prompt.empty()) {
                if (warnings)
                    warnings->push_back("agent " + d.name +
                                        " has no body (the system prompt); ignored");
                continue;
            }
            if (std::find(seen.begin(), seen.end(), d.name) != seen.end()) continue;
            seen.push_back(d.name);
            out.push_back(std::move(d));
        }
    }
    return out;
}

Definition general_purpose() {
    Definition d;
    d.name = "general";
    d.description =
        "General-purpose agent for open-ended work: searching a codebase, "
        "investigating a question across many files, or carrying out a "
        "self-contained subtask. Has the full tool set.";
    d.system_prompt =
        "You are a subagent working on one focused task on behalf of another "
        "agent. Use your tools to find the answer yourself rather than asking "
        "questions -- there is nobody to answer them.\n"
        "\n"
        "Your final message is the entire result: it is returned verbatim to the "
        "agent that called you, which cannot see your tool calls or your "
        "reasoning. So state the conclusion and the evidence for it, include "
        "exact file paths, line numbers and identifiers, and do not say things "
        "like \"as shown above\". Be complete but do not pad.";
    return d;
}

// ---------------------------------------------------------------------------
// Running one
// ---------------------------------------------------------------------------

namespace {

// Build the child's tool set: everything the parent has, minus the spawning
// tools (so an agent cannot recurse), optionally narrowed to a named list.
ToolRegistry build_child_tools(const ToolRegistry& parent,
                               const std::vector<std::string>& allow,
                               std::vector<std::string>* unknown) {
    ToolRegistry child;
    for (const std::string& name : parent.names()) {
        if (name == "task" || name == "task_batch") continue;   // no recursion
        if (!allow.empty() &&
            std::find(allow.begin(), allow.end(), name) == allow.end())
            continue;
        if (const Tool* t = parent.find(name)) child.add(*t);
    }
    if (unknown && !allow.empty()) {
        for (const std::string& want : allow)
            if (!parent.find(want)) unknown->push_back(want);
    }
    return child;
}

} // namespace

Result run(const Definition& def, const std::string& prompt, const Host& host) {
    Result res;
    res.agent = def.name;

    if (!host.config || !host.parent_tools) {
        res.error = "subagent host is not configured";
        return res;
    }

    // The child gets its own Config so a model override cannot leak back.
    Config cfg = *host.config;
    if (!def.model.empty()) cfg.model = def.model;
    if (def.max_turns) cfg.max_turns = *def.max_turns;
    // A subagent should not be able to spend the whole budget on its own; the
    // parent's cap still applies through the shared usage roll-up.
    res.model = cfg.model;

    Client client(cfg);

    std::vector<std::string> unknown;
    ToolRegistry tools = build_child_tools(*host.parent_tools, def.tools, &unknown);
    if (!unknown.empty())
        log_line("subagent " + def.name + ": unknown tools requested: " +
                 join(unknown, ", "));

    Agent agent(client, tools, cfg);
    agent.set_cwd(host.cwd);

    // System prompt: the platform context the parent already paid to assemble,
    // then this agent's own instructions.
    std::string system;
    if (def.inherit_context && !host.base_system.empty())
        system = host.base_system + "\n\n---\n\n";
    system += def.system_prompt;
    agent.set_system_prompt(system);

    Agent::Events ev;
    if (host.note) {
        ev.on_tool_start = [&](const ToolCall& tc) {
            host.note("  [" + def.name + "] " + tc.name);
        };
    }
    if (host.approve) {
        // Serialised so a fan-out cannot interleave prompts.
        ev.approve = [&](const std::string& name, ToolKind kind,
                         const ToolPreview& pv) -> bool {
            std::unique_lock<std::mutex> lk;
            if (host.approve_mutex)
                lk = std::unique_lock<std::mutex>(*host.approve_mutex);
            ToolPreview labelled = pv;
            labelled.title = "[" + def.name + "] " + labelled.title;
            return host.approve(name, kind, labelled);
        };
    }

    Agent::RunResult r = agent.run(prompt, ev, host.cancel);

    res.usage = r.usage;
    res.rounds = r.rounds;
    res.tool_calls = r.tool_calls;
    res.text = r.final_text;
    res.ok = r.ok;
    res.error = r.error;

    if (host.usage_mutex && host.shared_usage) {
        std::lock_guard<std::mutex> lk(*host.usage_mutex);
        host.shared_usage->add(r.usage);
    }

    if (res.ok && trim(res.text).empty())
        res.text = "(the subagent finished without producing a final message)";
    return res;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

namespace {

std::string describe_agents(const std::vector<Definition>& defs) {
    std::string out;
    for (const Definition& d : defs) {
        out += "  " + d.name;
        if (!d.model.empty()) out += " (model: " + d.model + ")";
        if (!d.description.empty()) out += " -- " + d.description;
        out += "\n";
    }
    return out;
}

const Definition* find_def(const std::vector<Definition>& defs,
                           const std::string& name) {
    for (const Definition& d : defs)
        if (d.name == name) return &d;
    return nullptr;
}

std::string format_result(const Result& r) {
    std::string out;
    if (!r.ok) {
        out = "Subagent '" + r.agent + "' failed: " + r.error;
        if (!r.text.empty()) out += "\n\nPartial result:\n" + r.text;
        return out;
    }
    out = r.text;
    char foot[256];
    std::snprintf(foot, sizeof(foot),
                  "\n\n[%s via %s: %d round%s, %d tool call%s, %lld tokens, $%.4f]",
                  r.agent.c_str(), r.model.c_str(), r.rounds,
                  r.rounds == 1 ? "" : "s", r.tool_calls,
                  r.tool_calls == 1 ? "" : "s",
                  static_cast<long long>(r.usage.total_tokens), r.usage.cost);
    out += foot;
    return out;
}

} // namespace

void add_tools(ToolRegistry& registry, const Host& host_in,
               const std::vector<Definition>& definitions) {
    // Copy so the lambdas own their state for the process lifetime.
    auto defs = std::make_shared<std::vector<Definition>>();
    defs->push_back(general_purpose());
    for (const Definition& d : definitions)
        if (d.name != "general") defs->push_back(d);

    auto host = std::make_shared<Host>(host_in);

    std::string roster = describe_agents(*defs);

    {
        Tool t;
        t.spec.name = "task";
        t.spec.description =
            "Delegate a self-contained task to a subagent. The subagent works in "
            "its own context with its own tools and returns only its final "
            "message, so use this to keep large investigations out of your own "
            "context: searching a codebase, reading many files to answer one "
            "question, or any subtask whose intermediate steps you do not need to "
            "see.\n"
            "\n"
            "Give it everything it needs in the prompt -- it cannot see your "
            "conversation and cannot ask you questions. Say exactly what to "
            "return.\n"
            "\n"
            "Available agents:\n" + roster +
            "\n"
            "You may also set 'model' to run the subagent on a different model "
            "than your own; a cheaper model is often the right choice for bulk "
            "searching or summarising.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "description": {"type": "string", "description": "A short label for this task, three to five words."},
                "prompt":      {"type": "string", "description": "The complete task. The subagent sees only this."},
                "agent":       {"type": "string", "description": "Which agent to use. Defaults to 'general'."},
                "model":       {"type": "string", "description": "Override the model for this subagent, e.g. a cheaper one for bulk work."}
            },
            "required": ["prompt"]
        })");
        // Spawning costs money and can run tools, so it goes through the gate.
        t.kind = ToolKind::Execute;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"task  " + jstr(a, "agent", "general"),
                               jstr(a, "description", json_preview(jstr(a, "prompt"), 120))};
        };
        t.handler = [defs, host](const json& a, ToolContext& ctx) -> ToolResult {
            std::string prompt = jstr(a, "prompt");
            if (prompt.empty()) return ToolResult::err("'prompt' is required");

            std::string want = jstr(a, "agent", "general");
            const Definition* d = find_def(*defs, want);
            if (!d)
                return ToolResult::err("unknown agent '" + want + "'. Available: " +
                                       describe_agents(*defs));

            Definition def = *d;
            if (std::string m = jstr(a, "model"); !m.empty()) def.model = m;

            Host h = *host;
            h.cwd = ctx.cwd;
            if (ctx.cancel) h.cancel = ctx.cancel;
            if (ctx.note) h.note = ctx.note;

            if (ctx.note)
                ctx.note("subagent " + def.name + ": " +
                         jstr(a, "description", elide(prompt, 60)));

            Result r = run(def, prompt, h);
            return r.ok ? ToolResult::ok(format_result(r))
                        : ToolResult::err(format_result(r));
        };
        registry.add(std::move(t));
    }

    {
        Tool t;
        t.spec.name = "task_batch";
        t.spec.description =
            "Run several subagents at once and collect all of their results. Use "
            "this when the subtasks are independent -- searching four different "
            "areas, checking several files, evaluating a few approaches. It is "
            "much faster than issuing 'task' repeatedly, because the calls "
            "overlap rather than queueing.\n"
            "\n"
            "Each entry may name its own agent and its own model.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "tasks": {
                    "type": "array",
                    "description": "The subtasks to run concurrently.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "description": {"type": "string", "description": "Short label."},
                            "prompt":      {"type": "string", "description": "The complete task."},
                            "agent":       {"type": "string", "description": "Agent name. Defaults to 'general'."},
                            "model":       {"type": "string", "description": "Model override for this one."}
                        },
                        "required": ["prompt"]
                    }
                }
            },
            "required": ["tasks"]
        })");
        t.kind = ToolKind::Execute;
        t.source = "builtin";
        t.preview = [](const json& a) {
            const json* ts = jptr(a, "tasks");
            size_t n = (ts && ts->is_array()) ? ts->size() : 0;
            std::string detail;
            if (ts && ts->is_array())
                for (const json& e : *ts)
                    detail += "  - " + jstr(e, "description",
                                            json_preview(jstr(e, "prompt"), 60)) + "\n";
            return ToolPreview{"task_batch  " + std::to_string(n) + " agents", detail};
        };
        t.handler = [defs, host](const json& a, ToolContext& ctx) -> ToolResult {
            const json* ts = jptr(a, "tasks");
            if (!ts || !ts->is_array() || ts->empty())
                return ToolResult::err("'tasks' must be a non-empty array");
            if (ts->size() > 12)
                return ToolResult::err("too many tasks in one batch (max 12)");

            struct Item { Definition def; std::string prompt, label; };
            std::vector<Item> items;
            for (const json& e : *ts) {
                std::string prompt = jstr(e, "prompt");
                if (prompt.empty())
                    return ToolResult::err("every task needs a 'prompt'");
                std::string want = jstr(e, "agent", "general");
                const Definition* d = find_def(*defs, want);
                if (!d)
                    return ToolResult::err("unknown agent '" + want + "'");
                Definition def = *d;
                if (std::string m = jstr(e, "model"); !m.empty()) def.model = m;
                items.push_back({def, prompt,
                                 jstr(e, "description", elide(prompt, 50))});
            }

            Host base = *host;
            base.cwd = ctx.cwd;
            if (ctx.cancel) base.cancel = ctx.cancel;
            if (ctx.note) base.note = ctx.note;

            if (ctx.note)
                ctx.note("fanning out " + std::to_string(items.size()) + " subagents");

            std::vector<Result> results(items.size());
            const size_t limit =
                std::max<size_t>(1, static_cast<size_t>(base.max_parallel));

            // Run in waves rather than all at once: each subagent holds a curl
            // connection and a thread, and this machine has two cores.
            for (size_t start = 0; start < items.size(); start += limit) {
                size_t end = std::min(items.size(), start + limit);
                std::vector<std::thread> threads;
                for (size_t i = start; i < end; i++) {
                    threads.emplace_back([&, i]() {
                        results[i] = run(items[i].def, items[i].prompt, base);
                    });
                }
                for (std::thread& th : threads) th.join();
                if (ctx.cancel && ctx.cancel->load()) break;
            }

            std::string out;
            Usage total;
            int failures = 0;
            for (size_t i = 0; i < items.size(); i++) {
                out += "===== " + items[i].label + " (" + results[i].agent;
                if (!results[i].model.empty()) out += " / " + results[i].model;
                out += ") =====\n";
                out += format_result(results[i]);
                out += "\n\n";
                total.add(results[i].usage);
                if (!results[i].ok) failures++;
            }
            char foot[192];
            std::snprintf(foot, sizeof(foot),
                          "[%zu subagents, %d failed, %lld tokens, $%.4f total]",
                          items.size(), failures,
                          static_cast<long long>(total.total_tokens), total.cost);
            out += foot;

            // A partial failure is still a useful result, so only report an
            // error when every one of them failed.
            return (failures == static_cast<int>(items.size()))
                       ? ToolResult::err(out)
                       : ToolResult::ok(out);
        };
        registry.add(std::move(t));
    }
}

} // namespace ppcode::subagent

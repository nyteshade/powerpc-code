#include "headless.hpp"

#include <algorithm>
#include <cstdio>
#include <unistd.h>

namespace ppcode {

namespace {

std::string read_stdin_all() {
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) out.append(buf, n);
    return out;
}

// One JSONL record, flushed immediately so a consumer reading our pipe sees
// events as they happen rather than at exit.
void emit(const json& j) {
    std::string s = j.dump();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace

int run_headless(Agent& agent, const Config& cfg, const HeadlessOptions& opt) {
    std::string prompt = opt.prompt;
    if (prompt.empty()) {
        if (isatty(STDIN_FILENO)) {
            std::fprintf(stderr,
                         "ppcode: no prompt given. Pass -p \"...\" or pipe text on stdin.\n");
            return 2;
        }
        prompt = trim(read_stdin_all());
    }
    if (prompt.empty()) {
        std::fprintf(stderr, "ppcode: empty prompt\n");
        return 2;
    }

    if (!opt.resume_path.empty()) {
        std::string text, err;
        if (!read_file_text(opt.resume_path, &text, &err)) {
            std::fprintf(stderr, "ppcode: cannot resume: %s\n", err.c_str());
            return 2;
        }
        try {
            if (!agent.from_json(json::parse(text), &err)) {
                std::fprintf(stderr, "ppcode: cannot resume: %s\n", err.c_str());
                return 2;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ppcode: cannot resume: %s\n", e.what());
            return 2;
        }
    }

    const bool jsonl = opt.format == OutputFormat::StreamJson;
    const bool textout = opt.format == OutputFormat::Text;

    // Collected for the --json summary.
    json tool_log = json::array();
    std::string assistant_text;

    Agent::Events ev;

    ev.on_text = [&](const std::string& delta) {
        assistant_text += delta;
        if (textout) {
            std::fwrite(delta.data(), 1, delta.size(), stdout);
            std::fflush(stdout);
        } else if (jsonl) {
            emit({{"type", "text"}, {"text", delta}});
        }
    };

    ev.on_status = [&](const std::string& s) {
        if (opt.quiet) return;
        if (jsonl) emit({{"type", "status"}, {"message", s}});
        else if (!s.empty()) std::fprintf(stderr, "[%s]\n", s.c_str());
    };

    ev.on_error = [&](const std::string& s) {
        if (jsonl) emit({{"type", "error"}, {"message", s}});
        else std::fprintf(stderr, "ppcode: %s\n", s.c_str());
    };

    ev.on_tool_start = [&](const ToolCall& tc) {
        if (jsonl) {
            emit({{"type", "tool_start"},
                  {"id", tc.id},
                  {"name", tc.name},
                  {"arguments", tc.arguments}});
        } else if (!opt.quiet) {
            std::fprintf(stderr, "[tool: %s %s]\n", tc.name.c_str(),
                         json_preview(tc.arguments, 120).c_str());
        }
    };

    ev.on_tool_done = [&](const ToolCall& tc, const ToolResult& tr) {
        if (jsonl) {
            emit({{"type", "tool_result"},
                  {"id", tc.id},
                  {"name", tc.name},
                  {"is_error", tr.is_error},
                  {"content", tr.content}});
        } else if (!opt.quiet && tr.is_error) {
            std::fprintf(stderr, "[tool %s failed: %s]\n", tc.name.c_str(),
                         json_preview(tr.content, 200).c_str());
        }
        tool_log.push_back({{"name", tc.name},
                            {"arguments", tc.arguments},
                            {"is_error", tr.is_error},
                            {"content", tr.content}});
    };

    // Approval policy for an unattended run. Read-only tools never reach here.
    // Everything else is refused unless explicitly permitted, so a script that
    // forgets --yolo fails safe instead of rewriting the disk.
    ev.approve = [&](const std::string& name, ToolKind kind,
                     const ToolPreview& pv) -> bool {
        if (std::find(opt.deny_tools.begin(), opt.deny_tools.end(), name) !=
            opt.deny_tools.end())
            return false;
        if (opt.yolo) return true;
        if (std::find(opt.allow_tools.begin(), opt.allow_tools.end(), name) !=
            opt.allow_tools.end())
            return true;
        if (!opt.quiet && !jsonl)
            std::fprintf(stderr,
                         "[refused %s -- not permitted in headless mode; "
                         "pass --allow-tool %s or --yolo]\n",
                         name.c_str(), name.c_str());
        if (jsonl)
            emit({{"type", "tool_denied"}, {"name", name}, {"title", pv.title}});
        return false;
    };

    std::atomic<bool> cancel{false};
    Agent::RunResult r = agent.run(prompt, ev, &cancel);

    if (textout && !assistant_text.empty() && assistant_text.back() != '\n')
        std::fputc('\n', stdout);

    if (!opt.save_path.empty()) {
        std::string err;
        if (!write_file_text(opt.save_path, agent.to_json().dump(2) + "\n", &err))
            std::fprintf(stderr, "ppcode: could not save session: %s\n", err.c_str());
    }

    if (opt.format == OutputFormat::Json) {
        json out;
        out["ok"] = r.ok;
        out["text"] = assistant_text;
        out["rounds"] = r.rounds;
        out["tool_calls"] = r.tool_calls;
        out["tools"] = tool_log;
        out["model"] = cfg.model;
        out["usage"] = {{"prompt_tokens", r.usage.prompt_tokens},
                        {"completion_tokens", r.usage.completion_tokens},
                        {"total_tokens", r.usage.total_tokens},
                        {"cost", r.usage.cost}};
        if (!r.error.empty()) out["error"] = r.error;
        emit(out);
    } else if (jsonl) {
        json out;
        out["type"] = "done";
        out["ok"] = r.ok;
        out["rounds"] = r.rounds;
        out["tool_calls"] = r.tool_calls;
        out["usage"] = {{"prompt_tokens", r.usage.prompt_tokens},
                        {"completion_tokens", r.usage.completion_tokens},
                        {"total_tokens", r.usage.total_tokens},
                        {"cost", r.usage.cost}};
        if (!r.error.empty()) out["error"] = r.error;
        emit(out);
    } else if (!opt.quiet && r.usage.total_tokens > 0) {
        std::fprintf(stderr, "[%lld tokens, %d round%s]\n",
                     static_cast<long long>(r.usage.total_tokens), r.rounds,
                     r.rounds == 1 ? "" : "s");
    }

    return r.ok ? 0 : 1;
}

} // namespace ppcode

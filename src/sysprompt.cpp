#include "sysprompt.hpp"

#include "yaml.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include <unistd.h>

namespace fs = std::filesystem;

namespace ppcode::sysprompt {

size_t Doc::est_tokens() const { return envinfo::estimate_tokens(body); }

namespace {

// Directory containing the running executable, so a knowledge/ folder next to
// the binary is found without configuration.
std::string exe_dir() {
    // Darwin has _NSGetExecutablePath, but /proc is absent and argv[0] is not
    // available here. Fall back to the cwd-relative layout, which covers the
    // normal "run it from the source tree" case.
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    return ec ? "." : p.string();
}

} // namespace

std::vector<std::string> knowledge_dirs() {
    std::vector<std::string> dirs;
    if (const char* e = std::getenv("PPCODE_KNOWLEDGE_DIR"); e && *e)
        dirs.push_back(e);
    if (const char* h = std::getenv("HOME"); h && *h)
        dirs.push_back(std::string(h) + "/.config/ppcode/knowledge");
    dirs.push_back(exe_dir() + "/knowledge");
    dirs.push_back("knowledge");
    return dirs;
}

std::vector<Doc> load_docs(std::vector<std::string>* warnings) {
    std::vector<Doc> docs;
    std::vector<std::string> seen;

    for (const std::string& dir : knowledge_dirs()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string name = e.path().filename().string();
            if (ends_with(to_lower(name), ".md")) files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());

        for (const fs::path& f : files) {
            std::string id = f.stem().string();
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;

            std::string text, err;
            if (!read_file_text(f.string(), &text, &err)) {
                if (warnings) warnings->push_back("knowledge: " + err);
                continue;
            }

            Doc d;
            d.id = id;
            d.path = f.string();
            d.title = id;

            std::string front, body, ferr;
            if (!yaml::split_frontmatter(text, &front, &body, &ferr)) {
                if (warnings)
                    warnings->push_back("knowledge " + id + ": " + ferr);
                body = text;
            }
            d.body = trim(body);

            if (!trim(front).empty()) {
                json meta;
                std::string yerr;
                if (yaml::parse(front, &meta, &yerr) && meta.is_object()) {
                    d.title = jstr(meta, "title", d.title);
                    d.priority = static_cast<int>(jint(meta, "priority", d.priority));
                    d.min_context = jint(meta, "min_context", 0);
                    if (const json* t = jptr(meta, "tags"); t && t->is_array())
                        for (const json& s : *t)
                            if (s.is_string()) d.tags.push_back(s.get<std::string>());
                } else if (warnings) {
                    warnings->push_back("knowledge " + id + " frontmatter: " + yerr);
                }
            }

            if (d.body.empty()) continue;
            seen.push_back(id);
            docs.push_back(std::move(d));
        }
    }

    std::stable_sort(docs.begin(), docs.end(),
                     [](const Doc& a, const Doc& b) { return a.priority < b.priority; });
    return docs;
}

// ---------------------------------------------------------------------------

namespace {

std::string base_instructions(const Inputs& in) {
    std::string s =
        "You are ppcode, a coding assistant running in a terminal on a vintage "
        "Macintosh. You help write, build, and debug software directly on this "
        "machine.\n"
        "\n"
        "Working method:\n"
        "- Use your tools rather than guessing. Read a file before editing it.\n"
        "- Match the surrounding code's style, naming, and comment density.\n"
        "- Prefer a small diff and the exact command over long explanations.\n"
        "- Verify your work: compile it, run it, and report what actually "
        "happened rather than what should have happened.\n";

    auto has = [&](const char* name) {
        return std::find(in.tool_names.begin(), in.tool_names.end(), name) !=
               in.tool_names.end();
    };

    if (has("multi_edit"))
        s += "- This is slow hardware and every round trip costs real time. "
             "Batch work: use multi_edit for several changes to one file and "
             "read_many_files when exploring.\n";
    if (has("run_background"))
        s += "- The bash tool has a timeout. Anything that could run for more "
             "than a couple of minutes -- a large compile, a port install, a full "
             "test suite -- must go through run_background, then be checked with "
             "job_output. Do not poll it tightly; give it real time to progress.\n";
    if (has("todo_write"))
        s += "- For any task of more than a few steps, keep a plan with "
             "todo_write and update it as you go.\n";
    if (has("web_search") || has("web_fetch"))
        s += "- You can search and read the web. Use it when you need current "
             "documentation or are unsure about an API on this old platform, "
             "rather than guessing from memory.\n";

    return s;
}

} // namespace

Result build(const Inputs& in) {
    Result out;
    if (!in.cfg) return out;

    const int64_t ctx = in.context_tokens > 0 ? in.context_tokens
                                              : 128000;   // conservative default
    const size_t budget =
        static_cast<size_t>(static_cast<double>(ctx) * in.budget_fraction);

    // 1. The operator's prompt, or ours.
    std::string text;
    if (!in.cfg->system_prompt.empty()) {
        text = in.cfg->system_prompt;
    } else {
        text = base_instructions(in);
    }

    // 2. Where we are.
    if (!in.cwd.empty()) text += "\nWorking directory: " + in.cwd + "\n";
    if (!in.model_id.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Model: %s (context %lld tokens)\n",
                      in.model_id.c_str(), static_cast<long long>(ctx));
        text += buf;
    }
    if (in.model_supports_images)
        text += "You can see images. The user may attach screenshots or photos.\n";

    // 3. The machine. This is the highest-value section, so it is sized first
    //    and gets its own share of the budget.
    envinfo::Detail detail = envinfo::Detail::None;
    if (in.probe && in.probe->ok) {
        detail = in.env_detail ? *in.env_detail
                               : envinfo::choose_detail(*in.probe, ctx,
                                                        in.budget_fraction * 0.6);
        std::string env = envinfo::render(*in.probe, detail);
        if (!env.empty()) text += "\n" + env;
    }
    out.env_detail = detail;

    // 4. Knowledge documents, highest priority first, until the budget is spent.
    if (in.include_knowledge && detail != envinfo::Detail::None) {
        std::vector<Doc> docs = load_docs(nullptr);
        size_t used = envinfo::estimate_tokens(text);

        for (const Doc& d : docs) {
            if (d.min_context > 0 && ctx < d.min_context) {
                out.skipped_docs.push_back(d.id + " (needs " +
                                           std::to_string(d.min_context) + " ctx)");
                continue;
            }
            size_t cost = d.est_tokens();
            if (used + cost > budget) {
                out.skipped_docs.push_back(d.id + " (budget)");
                continue;
            }
            text += "\n\n" + d.body;
            if (!ends_with(text, "\n")) text += "\n";
            used += cost;
            out.included_docs.push_back(d.id);
        }
    }

    out.text = text;
    out.est_tokens = envinfo::estimate_tokens(text);
    return out;
}

} // namespace ppcode::sysprompt

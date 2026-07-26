#include "builderr.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace ppcode::builderr {

std::string Diagnostic::format() const {
    std::string out = file;
    if (line > 0) {
        out += ":" + std::to_string(line);
        if (column > 0) out += ":" + std::to_string(column);
    }
    if (!severity.empty()) out += ": " + severity;
    out += ": " + message;
    return out;
}

int Report::error_count() const {
    int n = 0;
    for (const Diagnostic& d : diagnostics) if (d.severity == "error") n++;
    return n + static_cast<int>(link_errors.size());
}

int Report::warning_count() const {
    int n = 0;
    for (const Diagnostic& d : diagnostics) if (d.severity == "warning") n++;
    return n;
}

namespace {

// A GCC/clang diagnostic line: path:line[:col]: severity: message
// The path may contain colons on this platform only in odd cases, so scan from
// the left for the first colon followed by digits.
bool parse_diagnostic(const std::string& line, Diagnostic* out) {
    size_t pos = 0;
    while (true) {
        size_t colon = line.find(':', pos);
        if (colon == std::string::npos || colon + 1 >= line.size()) return false;
        if (!std::isdigit(static_cast<unsigned char>(line[colon + 1]))) {
            pos = colon + 1;
            continue;
        }

        size_t i = colon + 1;
        int lineno = 0;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
            lineno = lineno * 10 + (line[i] - '0');
            i++;
        }
        int col = 0;
        if (i < line.size() && line[i] == ':' && i + 1 < line.size() &&
            std::isdigit(static_cast<unsigned char>(line[i + 1]))) {
            i++;
            while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
                col = col * 10 + (line[i] - '0');
                i++;
            }
        }
        if (i >= line.size() || line[i] != ':') { pos = colon + 1; continue; }
        i++;

        std::string rest = trim(line.substr(i));
        std::string sev;
        for (const char* s : {"error", "warning", "note", "fatal error"}) {
            std::string prefix = std::string(s) + ":";
            if (starts_with(rest, prefix)) {
                sev = (std::string(s) == "fatal error") ? "error" : s;
                rest = trim(rest.substr(prefix.size()));
                break;
            }
        }
        if (sev.empty()) return false;   // a bare path:line: is not a diagnostic

        out->file = trim(line.substr(0, colon));
        out->line = lineno;
        out->column = col;
        out->severity = sev;
        out->message = rest;
        return true;
    }
}

bool is_link_error(const std::string& line) {
    std::string l = to_lower(line);
    return starts_with(l, "ld:") ||
           l.find("undefined symbols") != std::string::npos ||
           l.find("symbol(s) not found") != std::string::npos ||
           l.find("duplicate symbol") != std::string::npos ||
           l.find("can't locate file for:") != std::string::npos ||
           l.find("collect2:") == 0;
}

} // namespace

Report parse(const std::string& output) {
    Report r;
    std::vector<std::string> lines = split(output, '\n');

    bool in_undefined_block = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& raw = lines[i];
        std::string line = raw;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        std::string t = trim(line);
        if (t.empty()) continue;

        // Explicit outcome markers.
        if (t.find("** BUILD SUCCEEDED **") != std::string::npos) r.succeeded = true;
        if (t.find("** BUILD FAILED **") != std::string::npos) r.failed = true;

        // "Undefined symbols:" is followed by an indented list; capture it as a
        // unit rather than as unrelated lines.
        if (to_lower(t).find("undefined symbols") != std::string::npos) {
            in_undefined_block = true;
            r.link_errors.push_back(t);
            continue;
        }
        if (in_undefined_block) {
            // The block continues while lines are indented.
            if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
                if (r.link_errors.size() < 40) r.link_errors.push_back(t);
                continue;
            }
            in_undefined_block = false;
        }

        Diagnostic d;
        if (parse_diagnostic(t, &d)) {
            // Compilers often echo the offending source line next, sometimes
            // with a caret line after it. Keep the source line as context.
            if (i + 1 < lines.size()) {
                std::string next = lines[i + 1];
                std::string nt = trim(next);
                Diagnostic probe;
                if (!nt.empty() && !parse_diagnostic(nt, &probe) &&
                    nt.find_first_not_of("^~ \t") != std::string::npos &&
                    !is_link_error(nt)) {
                    d.context = elide(nt, 160);
                }
            }
            r.diagnostics.push_back(std::move(d));
            continue;
        }

        if (is_link_error(t)) {
            if (r.link_errors.size() < 40) r.link_errors.push_back(elide(t, 200));
            continue;
        }

        // make failure summary
        if (t.find("*** [") != std::string::npos && t.find("Error") != std::string::npos) {
            r.make_failures.push_back(elide(t, 200));
            r.failed = true;
            continue;
        }
        if (starts_with(t, "make: ***") || starts_with(t, "gmake: ***")) {
            r.make_failures.push_back(elide(t, 200));
            r.failed = true;
        }
    }
    return r;
}

std::string Report::summarise(size_t max_diagnostics) const {
    std::string out;
    int errs = error_count(), warns = warning_count();

    if (succeeded && errs == 0) {
        out += "BUILD SUCCEEDED";
        if (warns > 0) out += " with " + std::to_string(warns) + " warning" +
                              (warns == 1 ? "" : "s");
        out += "\n";
    } else if (failed || errs > 0) {
        out += "BUILD FAILED: " + std::to_string(errs) + " error" +
               (errs == 1 ? "" : "s");
        if (warns > 0) out += ", " + std::to_string(warns) + " warning" +
                              (warns == 1 ? "" : "s");
        out += "\n";
    } else {
        out += std::to_string(errs) + " errors, " + std::to_string(warns) +
               " warnings\n";
    }
    if (exit_code >= 0) out += "exit code " + std::to_string(exit_code) + "\n";

    // Errors first: a warning is rarely what the caller needs to act on.
    std::vector<const Diagnostic*> ordered;
    for (const Diagnostic& d : diagnostics)
        if (d.severity == "error") ordered.push_back(&d);
    for (const Diagnostic& d : diagnostics)
        if (d.severity == "warning") ordered.push_back(&d);
    for (const Diagnostic& d : diagnostics)
        if (d.severity == "note") ordered.push_back(&d);

    if (!ordered.empty()) {
        out += "\n";
        size_t shown = 0;
        for (const Diagnostic* d : ordered) {
            if (shown >= max_diagnostics) {
                out += "... and " + std::to_string(ordered.size() - shown) +
                       " more\n";
                break;
            }
            out += "  " + d->format() + "\n";
            if (!d->context.empty()) out += "      " + d->context + "\n";
            shown++;
        }
    }

    if (!link_errors.empty()) {
        out += "\nLinker:\n";
        for (const std::string& l : link_errors) out += "  " + l + "\n";
    }
    if (!make_failures.empty()) {
        out += "\nMake:\n";
        for (const std::string& l : make_failures) out += "  " + l + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------

void add_tools(ToolRegistry& registry) {
    Tool t;
    t.spec.name = "build";
    t.spec.description =
        "Run a build command and get back structured diagnostics instead of raw "
        "log output: the errors and warnings with their file, line and message, "
        "plus linker and make failures.\n"
        "\n"
        "Prefer this over running the build through bash. A failing build here "
        "emits hundreds of lines, and you pay for all of them on every "
        "subsequent round; this returns a summary. The full tail is still "
        "available if you ask for it.\n"
        "\n"
        "For a build that may run longer than a couple of minutes, use "
        "run_background instead and parse its log with this tool's 'output' "
        "parameter.";
    t.spec.parameters = json::parse(R"({
        "type": "object",
        "properties": {
            "command":    {"type": "string",  "description": "Build command to run, e.g. 'gmake -j2' or 'xcodebuild -configuration Debug'. Omit if passing 'output' instead."},
            "cwd":        {"type": "string",  "description": "Directory to build in. Defaults to the working directory."},
            "output":     {"type": "string",  "description": "Parse this text instead of running anything. Use for output you already captured."},
            "timeout_ms": {"type": "integer", "description": "Kill the build after this long. Default 300000."},
            "include_raw":{"type": "boolean", "description": "Also return the tail of the raw output. Default false."}
        }
    })");
    t.kind = ToolKind::Execute;
    t.source = "builtin";
    t.preview = [](const json& a) {
        std::string c = jstr(a, "command");
        return ToolPreview{"build", c.empty() ? "(parse captured output)" : c};
    };
    t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
        std::string captured = jstr(a, "output");
        std::string cmd = jstr(a, "command");

        if (captured.empty() && cmd.empty())
            return ToolResult::err("give either 'command' to run or 'output' to parse");

        Report rep;
        std::string raw;

        if (!captured.empty()) {
            raw = captured;
            rep = parse(raw);
        } else {
            int64_t timeout = jint(a, "timeout_ms", 300000);
            if (timeout <= 0 || timeout > 600000) timeout = 300000;
            std::string cwd = jstr(a, "cwd");
            cwd = cwd.empty() ? ctx.cwd : resolve_path(cwd, ctx.cwd);

            if (ctx.note) ctx.note("building: " + elide(cmd, 100));

            CommandResult r = run_shell(cmd, cwd, static_cast<int>(timeout),
                                        1024 * 1024, ctx.cancel);
            if (r.spawn_failed) return ToolResult::err(r.error);
            raw = r.output;
            rep = parse(raw);
            rep.exit_code = r.exit_code;
            if (r.timed_out)
                return ToolResult::err(
                    "the build timed out after " + std::to_string(timeout) +
                    "ms and was killed. If it legitimately takes this long, start "
                    "it with run_background instead.\n\n" + rep.summarise());
            // An exit code is the ground truth when no marker was printed.
            if (r.exit_code != 0 && !rep.succeeded) rep.failed = true;
            if (r.exit_code == 0 && rep.error_count() == 0) rep.succeeded = true;
        }

        std::string out = rep.summarise();
        if (jbool(a, "include_raw", false)) {
            std::vector<std::string> lines = split(raw, '\n');
            if (lines.size() > 60) {
                size_t drop = lines.size() - 60;
                lines.erase(lines.begin(), lines.begin() + static_cast<long>(drop));
                out += "\nRaw output (last 60 lines of " +
                       std::to_string(drop + 60) + "):\n" + join(lines, "\n");
            } else {
                out += "\nRaw output:\n" + raw;
            }
        } else if (rep.diagnostics.empty() && rep.link_errors.empty() &&
                   rep.make_failures.empty() && !rep.succeeded) {
            // Nothing parsed and no success marker: show something rather than
            // an empty summary.
            out += "\nNo diagnostics were recognised. Tail of the output:\n";
            std::vector<std::string> lines = split(raw, '\n');
            size_t start = lines.size() > 30 ? lines.size() - 30 : 0;
            for (size_t i = start; i < lines.size(); i++) out += lines[i] + "\n";
        }

        return rep.failed && !rep.succeeded ? ToolResult::err(out)
                                            : ToolResult::ok(out);
    };
    registry.add(std::move(t));
}

} // namespace ppcode::builderr

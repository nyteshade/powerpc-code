#include "bundler.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace ppcode::bundle {

namespace {

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

bool is_system_path(const std::string& p) {
    return starts_with(p, "/usr/lib/") || starts_with(p, "/System/") ||
           starts_with(p, "/usr/local/lib/system");
}

// Already relative to the loading binary, so nothing to do.
bool is_relative_ref(const std::string& p) {
    return starts_with(p, "@executable_path") || starts_with(p, "@loader_path") ||
           starts_with(p, "@rpath");
}

std::string run(const std::string& cmd, int* exit_code = nullptr) {
    CommandResult r = run_shell(cmd, ".", 60000, 512 * 1024, nullptr);
    if (exit_code) *exit_code = r.exit_code;
    return r.output;
}

} // namespace

std::vector<DylibRef> dependencies(const std::string& binary) {
    std::vector<DylibRef> out;
    int rc = 0;
    std::string o = run("otool -L " + sh_quote(binary) + " 2>/dev/null", &rc);
    if (rc != 0) return out;

    bool first = true;
    for (const std::string& raw : split(o, '\n')) {
        std::string line = trim(raw);
        if (line.empty()) continue;
        // The first line is the file itself, not a dependency.
        if (first && ends_with(line, ":")) { first = false; continue; }
        first = false;
        // "path (compatibility version X, current version Y)"
        size_t paren = line.find(" (");
        if (paren == std::string::npos) continue;
        DylibRef d;
        d.path = trim(line.substr(0, paren));
        if (d.path.empty()) continue;
        d.is_system = is_system_path(d.path);
        out.push_back(std::move(d));
    }
    return out;
}

std::string install_name(const std::string& dylib) {
    int rc = 0;
    std::string o = run("otool -D " + sh_quote(dylib) + " 2>/dev/null", &rc);
    if (rc != 0) return "";
    std::vector<std::string> lines = split(o, '\n');
    // otool -D prints the file name then the install name.
    for (size_t i = 1; i < lines.size(); i++) {
        std::string t = trim(lines[i]);
        if (!t.empty()) return t;
    }
    return "";
}

RelocateResult relocate(const std::string& binary, const std::string& dest_dir,
                        const std::string& loader_prefix,
                        const std::vector<std::string>& prefixes, bool dry_run) {
    RelocateResult res;
    std::error_code ec;

    if (!fs::exists(binary, ec)) {
        res.error = "not found: " + binary;
        return res;
    }

    auto relocatable = [&](const std::string& p) {
        if (is_system_path(p) || is_relative_ref(p)) return false;
        if (p.empty() || p[0] != '/') return false;
        if (prefixes.empty()) return true;
        for (const std::string& pre : prefixes)
            if (starts_with(p, pre)) return true;
        return false;
    };

    // Walk the dependency graph breadth-first, collecting every library that
    // has to travel with the application. Missing this transitively is the
    // usual cause of a bundle that works here and nowhere else.
    std::set<std::string> to_copy;
    std::vector<std::string> queue{binary};
    std::set<std::string> seen{binary};

    while (!queue.empty()) {
        std::string current = queue.back();
        queue.pop_back();
        for (const DylibRef& d : dependencies(current)) {
            if (!relocatable(d.path)) continue;
            if (!fs::exists(d.path, ec)) continue;
            if (to_copy.insert(d.path).second && seen.insert(d.path).second)
                queue.push_back(d.path);
        }
    }

    std::string report;
    report += "binary:      " + binary + "\n";
    report += "destination: " + dest_dir + "\n";
    report += "loader path: " + loader_prefix + "\n";
    report += "\n" + std::to_string(to_copy.size()) +
              " library/libraries to bundle:\n";
    for (const std::string& p : to_copy) report += "  " + p + "\n";

    if (dry_run) {
        res.ok = true;
        res.report = report + "\n(dry run: nothing was copied or modified)";
        return res;
    }

    fs::create_directories(dest_dir, ec);
    if (ec) {
        res.error = "cannot create " + dest_dir + ": " + ec.message();
        return res;
    }

    // 1. Copy, and make writable -- MacPorts installs libraries read-only and
    //    install_name_tool cannot modify them in place.
    std::map<std::string, std::string> new_path;   // original -> in-bundle
    for (const std::string& src : to_copy) {
        std::string base = fs::path(src).filename().string();
        std::string dst = dest_dir + "/" + base;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            res.error = "could not copy " + src + ": " + ec.message();
            return res;
        }
        fs::permissions(dst, fs::perms::owner_write, fs::perm_options::add, ec);
        new_path[src] = loader_prefix + "/" + base;
        res.copied.push_back(base);
    }

    // 2. Give each copied library its new identity.
    for (const std::string& src : to_copy) {
        std::string dst = dest_dir + "/" + fs::path(src).filename().string();
        int rc = 0;
        run("install_name_tool -id " + sh_quote(new_path[src]) + " " +
                sh_quote(dst) + " 2>&1",
            &rc);
        if (rc != 0) {
            res.error = "install_name_tool -id failed for " + dst;
            return res;
        }
    }

    // 3. Rewrite every reference, in the executable and in the libraries
    //    themselves -- the libraries reference each other.
    std::vector<std::string> targets{binary};
    for (const std::string& src : to_copy)
        targets.push_back(dest_dir + "/" + fs::path(src).filename().string());

    for (const std::string& target : targets) {
        bool changed = false;
        for (const auto& [orig, replacement] : new_path) {
            bool refs = false;
            for (const DylibRef& d : dependencies(target))
                if (d.path == orig) refs = true;
            if (!refs) continue;

            int rc = 0;
            run("install_name_tool -change " + sh_quote(orig) + " " +
                    sh_quote(replacement) + " " + sh_quote(target) + " 2>&1",
                &rc);
            if (rc != 0) {
                res.error = "install_name_tool -change failed on " + target;
                return res;
            }
            changed = true;
        }
        if (changed) res.rewritten.push_back(fs::path(target).filename().string());
    }

    // 4. Verify. This is the step people skip, and it is the only way to know
    //    the bundle will run on a machine without MacPorts.
    for (const std::string& target : targets) {
        for (const DylibRef& d : dependencies(target)) {
            if (is_system_path(d.path) || is_relative_ref(d.path)) continue;
            if (d.path.empty() || d.path[0] != '/') continue;
            res.leaks.push_back(fs::path(target).filename().string() + " -> " + d.path);
        }
    }

    report += "\ncopied:    " + std::to_string(res.copied.size()) + "\n";
    report += "rewritten: " + std::to_string(res.rewritten.size()) + " binaries\n";
    if (res.leaks.empty()) {
        report += "\nVerified: no absolute non-system paths remain. This bundle "
                  "should run on a Mac without MacPorts.\n";
        res.ok = true;
    } else {
        report += "\nWARNING -- these references still point outside the bundle "
                  "and will fail on another machine:\n";
        for (const std::string& l : res.leaks) report += "  " + l + "\n";
        res.ok = false;
        res.error = "unresolved absolute paths remain";
    }
    res.report = report;
    return res;
}

// ---------------------------------------------------------------------------

BundleResult make_bundle(const BundleSpec& spec) {
    BundleResult res;
    std::error_code ec;

    if (spec.app_path.empty() || spec.executable.empty()) {
        res.error = "both app_path and executable are required";
        return res;
    }
    if (!fs::exists(spec.executable, ec)) {
        res.error = "executable not found: " + spec.executable;
        return res;
    }

    std::string name = spec.name;
    if (name.empty()) name = fs::path(spec.app_path).stem().string();
    std::string exe_name = fs::path(spec.executable).filename().string();

    std::string contents = spec.app_path + "/Contents";
    std::string macos = contents + "/MacOS";
    std::string resources = contents + "/Resources";
    std::string frameworks = contents + "/Frameworks";

    fs::create_directories(macos, ec);
    fs::create_directories(resources, ec);
    if (ec) {
        res.error = "cannot create the bundle: " + ec.message();
        return res;
    }

    fs::copy_file(spec.executable, macos + "/" + exe_name,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        res.error = "could not copy the executable: " + ec.message();
        return res;
    }
    fs::permissions(macos + "/" + exe_name,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add, ec);

    std::string report = "bundle: " + spec.app_path + "\n";

    // Info.plist. LSMinimumSystemVersion matters: without it the Finder will
    // happily launch this on a system whose frameworks it was not built for.
    std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>CFBundleName</key><string>" + name + "</string>\n"
        "\t<key>CFBundleDisplayName</key><string>" + name + "</string>\n"
        "\t<key>CFBundleExecutable</key><string>" + exe_name + "</string>\n"
        "\t<key>CFBundleIdentifier</key><string>" +
            (spec.identifier.empty() ? ("local." + to_lower(name)) : spec.identifier) +
            "</string>\n"
        "\t<key>CFBundleVersion</key><string>" + spec.version + "</string>\n"
        "\t<key>CFBundleShortVersionString</key><string>" + spec.version + "</string>\n"
        "\t<key>CFBundlePackageType</key><string>APPL</string>\n"
        "\t<key>CFBundleSignature</key><string>????</string>\n"
        "\t<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
        "\t<key>LSMinimumSystemVersion</key><string>" + spec.min_system + "</string>\n"
        "\t<key>NSPrincipalClass</key><string>NSApplication</string>\n";
    if (!spec.icon.empty())
        plist += "\t<key>CFBundleIconFile</key><string>" +
                 fs::path(spec.icon).filename().string() + "</string>\n";
    plist += "</dict>\n</plist>\n";

    std::string werr;
    if (!write_file_text(contents + "/Info.plist", plist, &werr)) {
        res.error = werr;
        return res;
    }
    write_file_text(contents + "/PkgInfo", "APPL????", nullptr);

    if (!spec.icon.empty() && fs::exists(spec.icon, ec)) {
        fs::copy_file(spec.icon,
                      resources + "/" + fs::path(spec.icon).filename().string(),
                      fs::copy_options::overwrite_existing, ec);
        report += "icon:   " + fs::path(spec.icon).filename().string() + "\n";
    }
    for (const std::string& r : spec.resources) {
        if (!fs::exists(r, ec)) continue;
        fs::copy(r, resources + "/" + fs::path(r).filename().string(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        report += "resource: " + fs::path(r).filename().string() + "\n";
    }

    if (spec.relocate_libs) {
        RelocateResult rr = relocate(macos + "/" + exe_name, frameworks,
                                     "@loader_path/../Frameworks",
                                     {"/opt/local", "/usr/local"}, false);
        report += "\n" + rr.report;
        if (!rr.ok && !rr.leaks.empty()) {
            // Still a usable bundle on this machine; say plainly that it is not
            // portable rather than pretending it succeeded.
            res.error = rr.error;
            res.report = report;
            return res;
        }
    }

    res.ok = true;
    res.report = report + "\nDone. Launch with: open " + spec.app_path + "\n";
    return res;
}

// ---------------------------------------------------------------------------

void add_tools(ToolRegistry& registry) {
    {
        Tool t;
        t.spec.name = "bundle_app";
        t.spec.description =
            "Assemble a .app bundle around a built executable: Contents/MacOS, "
            "an Info.plist, PkgInfo, resources, and by default the MacPorts "
            "libraries it needs, copied in and repointed so the application runs "
            "on a Mac that does not have MacPorts installed.\n"
            "\n"
            "Anything built here links against /opt/local by absolute path, so "
            "without this step the result runs on this machine and nowhere else.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "app_path":   {"type": "string", "description": "Bundle to create, e.g. build/MyApp.app"},
                "executable": {"type": "string", "description": "The built binary to put inside it."},
                "name":       {"type": "string", "description": "CFBundleName. Defaults to the bundle's file name."},
                "identifier": {"type": "string", "description": "CFBundleIdentifier, e.g. com.example.myapp"},
                "version":    {"type": "string", "description": "Version string. Default 1.0."},
                "icon":       {"type": "string", "description": "Optional .icns file."},
                "resources":  {"type": "array", "items": {"type": "string"},
                               "description": "Files or directories to copy into Resources."},
                "relocate":   {"type": "boolean", "description": "Bundle and repoint MacPorts libraries. Default true."}
            },
            "required": ["app_path", "executable"]
        })");
        t.kind = ToolKind::Mutate;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"bundle_app  " + jstr(a, "app_path"),
                               "from " + jstr(a, "executable")};
        };
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            BundleSpec s;
            s.app_path = resolve_path(jstr(a, "app_path"), ctx.cwd);
            s.executable = resolve_path(jstr(a, "executable"), ctx.cwd);
            s.name = jstr(a, "name");
            s.identifier = jstr(a, "identifier");
            s.version = jstr(a, "version", "1.0");
            if (std::string i = jstr(a, "icon"); !i.empty())
                s.icon = resolve_path(i, ctx.cwd);
            if (const json* r = jptr(a, "resources"); r && r->is_array())
                for (const json& e : *r)
                    if (e.is_string())
                        s.resources.push_back(resolve_path(e.get<std::string>(), ctx.cwd));
            s.relocate_libs = jbool(a, "relocate", true);

            if (ctx.note) ctx.note("bundling " + s.app_path);

            BundleResult r = make_bundle(s);
            return r.ok ? ToolResult::ok(r.report)
                        : ToolResult::err(r.error + "\n\n" + r.report);
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "relocate_libs";
        t.spec.description =
            "Copy the non-system dynamic libraries a binary depends on into a "
            "directory and rewrite every reference to be relative to the loader, "
            "so the result does not need MacPorts at runtime. Follows "
            "dependencies transitively -- forgetting that one library references "
            "another is the usual reason a bundle works here and fails "
            "elsewhere -- and verifies afterwards that no absolute paths remain.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "binary":      {"type": "string", "description": "Executable or dylib to process."},
                "destination": {"type": "string", "description": "Directory to copy libraries into."},
                "loader_path": {"type": "string", "description": "How the binary should refer to them. Default @loader_path/../Frameworks"},
                "prefixes":    {"type": "array", "items": {"type": "string"},
                                "description": "Only relocate libraries under these prefixes. Default /opt/local and /usr/local."},
                "dry_run":     {"type": "boolean", "description": "Report what would be done without changing anything."}
            },
            "required": ["binary", "destination"]
        })");
        t.kind = ToolKind::Mutate;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"relocate_libs  " + jstr(a, "binary"),
                               jbool(a, "dry_run", false) ? "dry run"
                                                          : "into " + jstr(a, "destination")};
        };
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            std::string bin = jstr(a, "binary");
            std::string dest = jstr(a, "destination");
            if (bin.empty() || dest.empty())
                return ToolResult::err("'binary' and 'destination' are required");

            std::vector<std::string> prefixes;
            if (const json* p = jptr(a, "prefixes"); p && p->is_array())
                for (const json& e : *p)
                    if (e.is_string()) prefixes.push_back(e.get<std::string>());
            if (prefixes.empty()) prefixes = {"/opt/local", "/usr/local"};

            if (ctx.note) ctx.note("relocating libraries for " + bin);

            RelocateResult r = relocate(
                resolve_path(bin, ctx.cwd), resolve_path(dest, ctx.cwd),
                jstr(a, "loader_path", "@loader_path/../Frameworks"), prefixes,
                jbool(a, "dry_run", false));

            return r.ok ? ToolResult::ok(r.report)
                        : ToolResult::err(r.error + "\n\n" + r.report);
        };
        registry.add(std::move(t));
    }
}

} // namespace ppcode::bundle

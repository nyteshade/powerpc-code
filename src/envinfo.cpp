#include "envinfo.hpp"

#include "http.hpp"      // version_string, get
#include "tools.hpp"     // run_shell

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <unistd.h>

namespace fs = std::filesystem;

namespace ppcode::envinfo {

namespace {

// The probe runs many small commands; keep each one on a short leash so a
// wedged tool cannot hang startup.
constexpr int kCmdTimeoutMs = 8000;
constexpr int kSlowCmdTimeoutMs = 30000;   // `port installed` is not fast here

std::string sh(const std::string& cmd, int timeout_ms = kCmdTimeoutMs) {
    CommandResult r = run_shell(cmd, ".", timeout_ms, 512 * 1024, nullptr);
    if (r.spawn_failed || r.timed_out) return "";
    return trim(r.output);
}

// First line only -- most --version output is multi-line and we want the gist.
std::string first_line(const std::string& s) {
    size_t nl = s.find('\n');
    return trim(nl == std::string::npos ? s : s.substr(0, nl));
}

std::string which(const std::string& prog) {
    // `command -v` avoids depending on a `which` binary being present.
    return sh("command -v " + prog + " 2>/dev/null");
}

bool exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string human_bytes(uint64_t b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    char buf[64];
    if (v >= 10 || u == 0) std::snprintf(buf, sizeof(buf), "%.0f %s", v, units[u]);
    else std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

std::string now_stamp() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

// Candidate toolchain binaries. Probing a name that is absent costs one failed
// `command -v`, which is cheap, so the list can be generous.
struct Candidate {
    const char* name;
    const char* version_cmd;   // %s is replaced by the resolved path
    const char* note;          // stated only if this tool is present
};

const Candidate kCompilers[] = {
    {"gcc-mp-15", "%s --version",
     "GCC 15, full C++23. Preferred for both C/C++ and Objective-C here."},
    {"g++-mp-15", "%s --version", ""},
    {"gcc-mp-14", "%s --version", "GCC 14 fallback."},
    {"g++-mp-14", "%s --version", ""},
    {"gcc-mp-13", "%s --version", ""},
    {"g++-mp-13", "%s --version", ""},
    {"clang-mp-3.3", "%s --version",
     "LLVM 3.3. Requires -fPIC on this platform: without it PPC codegen emits "
     "absolute addresses into dylib data symbols and binaries fault at runtime "
     "(or fail to link with \"absolute address to symbol ... in a different "
     "linkage unit\"). For Objective-C it also needs "
     "-fobjc-runtime=macosx-fragile-10.5; it defaults to the GNU runtime and "
     "otherwise fails with undefined _objc_lookup_class. Its libstdc++ is "
     "Leopard's, so C++ is effectively C++03 -- use gcc15 for modern C++."},
    {"clang++-mp-3.3", "%s --version", ""},
    {"clang", "%s --version", ""},
    {"gcc", "%s --version", "System GCC (Apple 4.0.1/4.2.1 era). Very old; prefer MacPorts gcc."},
    {"gcc-4.2", "%s --version", ""},
    {"g++-4.2", "%s --version", ""},
};

const Candidate kLanguages[] = {
    {"python3", "%s --version", ""},
    {"python2.7", "%s --version", ""},
    {"perl", "%s -e 'print $^V'", ""},
    {"ruby", "%s --version", ""},
    {"java", "%s -version 2>&1", ""},
    {"qjs", "%s --help 2>&1 | head -1",
     "QuickJS. The only JS runtime here -- there is no Node.js. It provides "
     "std/os modules but no Node compatibility layer, so npm packages will not "
     "run."},
    {"tclsh", "echo 'puts $tcl_version; exit' | %s", ""},
    {"php", "%s --version", ""},
};

const Candidate kBuildTools[] = {
    {"gmake", "%s --version", "GNU make. Use this, not /usr/bin/make."},
    {"make",  "%s --version", ""},
    {"cmake", "%s --version", ""},
    {"autoconf", "%s --version", ""},
    {"automake", "%s --version", ""},
    {"libtool", "%s --version", ""},
    {"pkg-config", "%s --version", ""},
    {"git", "%s --version", ""},
    {"ninja", "%s --version", ""},
    {"xcodebuild", "%s -version", ""},
    {"install_name_tool", "", "Needed to relocate dylib paths out of /opt/local."},
    {"otool", "", ""},
    {"lipo", "", ""},
};

// Things whose absence changes what a model should suggest.
const char* kCheckAbsent[] = {"node", "npm", "yarn", "brew", "docker", "podman",
                              "rustc", "cargo", "go", "swift"};

// Applications worth knowing about, with what to tell the user when missing.
struct AppCheck {
    const char* bundle;      // .app name, searched in the usual locations
    const char* label;
    const char* suggestion;  // empty when its absence needs no comment
};

const AppCheck kApps[] = {
    {"PowerFox", "PowerFox",
     "PowerFox is the most capable browser available for PowerPC Leopard in 2026 "
     "(UXP/Basilisk engine, TLS 1.3, WebGL). It is not installed here. If a modern "
     "browser is needed, the current release is 26.1.0 from "
     "https://powerfox.jazzzny.me/download.html -- note its PowerPC build is beta "
     "and has no JavaScript JIT yet, so script-heavy pages are slow."},
    {"TenFourFox", "TenFourFox", ""},
    {"Leopard WebKit", "Leopard WebKit", ""},
    {"Camino", "Camino", ""},
    {"Safari", "Safari", ""},
    {"Xcode", "Xcode.app", ""},
    {"Interface Builder", "Interface Builder", ""},
    {"iTerm", "iTerm", ""},
    {"TextMate", "TextMate", ""},
    {"TextWrangler", "TextWrangler", ""},
};

// Read CFBundleShortVersionString from an app bundle, best effort.
std::string bundle_version(const std::string& app_path) {
    std::string plist = app_path + "/Contents/Info.plist";
    if (!exists(plist)) return "";
    // defaults(1) handles both XML and binary plists on this OS.
    std::string v = sh("defaults read '" + app_path +
                       "/Contents/Info' CFBundleShortVersionString 2>/dev/null");
    if (v.empty())
        v = sh("defaults read '" + app_path +
               "/Contents/Info' CFBundleVersion 2>/dev/null");
    return first_line(v);
}

std::vector<ToolEntry> probe_group(const Candidate* list, size_t n) {
    std::vector<ToolEntry> out;
    for (size_t i = 0; i < n; i++) {
        const Candidate& c = list[i];
        std::string path = which(c.name);
        if (path.empty()) continue;

        ToolEntry e;
        e.name = c.name;
        e.path = first_line(path);
        if (c.version_cmd && *c.version_cmd) {
            char cmd[1024];
            std::snprintf(cmd, sizeof(cmd), c.version_cmd, e.path.c_str());
            e.version = first_line(sh(std::string(cmd) + " 2>&1"));
        }
        e.note = c.note ? c.note : "";
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------

Detail detail_from_string(const std::string& s, bool* ok) {
    if (ok) *ok = true;
    std::string v = to_lower(trim(s));
    if (v == "none" || v == "off" || v == "false") return Detail::None;
    if (v == "minimal" || v == "min")              return Detail::Minimal;
    if (v == "brief" || v == "short")              return Detail::Brief;
    if (v == "standard" || v == "normal" || v == "auto") return Detail::Standard;
    if (v == "full" || v == "all" || v == "verbose")     return Detail::Full;
    if (ok) *ok = false;
    return Detail::Standard;
}

std::string detail_to_string(Detail d) {
    switch (d) {
        case Detail::None:     return "none";
        case Detail::Minimal:  return "minimal";
        case Detail::Brief:    return "brief";
        case Detail::Standard: return "standard";
        case Detail::Full:     return "full";
    }
    return "standard";
}

// ---------------------------------------------------------------------------

json Probe::to_json() const {
    json j;
    j["schema"] = schema;
    j["ok"] = ok;
    j["probed_at"] = probed_at;

    j["hostname"] = hostname;
    j["machine_model"] = machine_model;
    j["model_name"] = model_name;
    j["cpu_brand"] = cpu_brand;
    j["cpu_speed"] = cpu_speed;
    j["arch"] = arch;
    j["cpu_count"] = cpu_count;
    j["memory_bytes"] = memory_bytes;
    j["big_endian"] = big_endian;

    j["os_name"] = os_name;
    j["os_version"] = os_version;
    j["os_build"] = os_build;
    j["kernel"] = kernel;

    j["xcode_version"] = xcode_version;
    j["sdks"] = sdks;

    j["ports_prefix"] = ports_prefix;
    j["ports_version"] = ports_version;
    j["ports_installed"] = ports_installed;
    j["ports"] = ports;

    auto dump_tools = [](const std::vector<ToolEntry>& v) {
        json arr = json::array();
        for (const ToolEntry& t : v)
            arr.push_back({{"name", t.name}, {"path", t.path},
                           {"version", t.version}, {"note", t.note}});
        return arr;
    };
    j["compilers"] = dump_tools(compilers);
    j["languages"] = dump_tools(languages);
    j["build_tools"] = dump_tools(build_tools);
    j["absent"] = absent;
    j["caveats"] = caveats;
    j["apps"] = apps;
    j["app_suggestions"] = app_suggestions;

    j["curl_version"] = curl_version;
    j["tls_backend"] = tls_backend;
    j["https_works"] = https_works;
    j["https_note"] = https_note;
    return j;
}

Probe Probe::from_json(const json& j) {
    Probe p;
    p.schema = static_cast<int>(jint(j, "schema", 0));
    p.ok = jbool(j, "ok", false);
    p.probed_at = jstr(j, "probed_at");

    p.hostname = jstr(j, "hostname");
    p.machine_model = jstr(j, "machine_model");
    p.model_name = jstr(j, "model_name");
    p.cpu_brand = jstr(j, "cpu_brand");
    p.cpu_speed = jstr(j, "cpu_speed");
    p.arch = jstr(j, "arch");
    p.cpu_count = static_cast<int>(jint(j, "cpu_count"));
    p.memory_bytes = static_cast<uint64_t>(jint(j, "memory_bytes"));
    p.big_endian = jbool(j, "big_endian");

    p.os_name = jstr(j, "os_name");
    p.os_version = jstr(j, "os_version");
    p.os_build = jstr(j, "os_build");
    p.kernel = jstr(j, "kernel");

    p.xcode_version = jstr(j, "xcode_version");
    p.ports_prefix = jstr(j, "ports_prefix");
    p.ports_version = jstr(j, "ports_version");
    p.ports_installed = static_cast<int>(jint(j, "ports_installed"));

    auto load_strs = [&](const char* key, std::vector<std::string>* out) {
        if (const json* a = jptr(j, key); a && a->is_array())
            for (const json& s : *a)
                if (s.is_string()) out->push_back(s.get<std::string>());
    };
    load_strs("sdks", &p.sdks);
    load_strs("ports", &p.ports);
    load_strs("absent", &p.absent);
    load_strs("caveats", &p.caveats);
    load_strs("apps", &p.apps);
    load_strs("app_suggestions", &p.app_suggestions);

    auto load_tools = [&](const char* key, std::vector<ToolEntry>* out) {
        if (const json* a = jptr(j, key); a && a->is_array()) {
            for (const json& t : *a) {
                ToolEntry e;
                e.name = jstr(t, "name");
                e.path = jstr(t, "path");
                e.version = jstr(t, "version");
                e.note = jstr(t, "note");
                if (!e.name.empty()) out->push_back(std::move(e));
            }
        }
    };
    load_tools("compilers", &p.compilers);
    load_tools("languages", &p.languages);
    load_tools("build_tools", &p.build_tools);

    p.curl_version = jstr(j, "curl_version");
    p.tls_backend = jstr(j, "tls_backend");
    p.https_works = jbool(j, "https_works");
    p.https_note = jstr(j, "https_note");
    return p;
}

std::string cache_path() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::string(xdg) + "/ppcode/envprobe.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.cache/ppcode/envprobe.json";
    return "/tmp/ppcode-envprobe.json";
}

// ---------------------------------------------------------------------------

static Probe do_probe() {
    Probe p;
    p.probed_at = now_stamp();

    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) p.hostname = host;

    // Hardware. sysctl names differ across Darwin versions, so tolerate misses.
    p.machine_model = sh("sysctl -n hw.model 2>/dev/null");
    p.arch          = sh("sysctl -n hw.machine 2>/dev/null");
    if (p.arch.empty()) p.arch = sh("uname -p 2>/dev/null");
    if (std::string n = sh("sysctl -n hw.ncpu 2>/dev/null"); !n.empty())
        p.cpu_count = std::atoi(n.c_str());
    if (std::string m = sh("sysctl -n hw.memsize 2>/dev/null"); !m.empty())
        p.memory_bytes = std::strtoull(m.c_str(), nullptr, 10);

    // system_profiler is slow but is the only place the marketing name and CPU
    // description live on PPC.
    std::string sp = sh("system_profiler SPHardwareDataType 2>/dev/null", kSlowCmdTimeoutMs);
    for (const std::string& raw : split(sp, '\n')) {
        std::string line = trim(raw);
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = trim(line.substr(0, c));
        std::string v = trim(line.substr(c + 1));
        if (v.empty()) continue;
        if (k == "Model Name")           p.model_name = v;
        else if (k == "Processor Name")   p.cpu_brand = v;
        else if (k == "Processor Speed")  p.cpu_speed = v;
        else if (k == "CPU Type")         { if (p.cpu_brand.empty()) p.cpu_brand = v; }
    }
    if (p.cpu_brand.empty())
        p.cpu_brand = sh("sysctl -n machdep.cpu.brand_string 2>/dev/null");

    {
        // Endianness of the running binary, not of the reported architecture.
        uint16_t v = 1;
        p.big_endian = (*reinterpret_cast<unsigned char*>(&v) == 0);
    }

    // Operating system
    p.os_name    = sh("sw_vers -productName 2>/dev/null");
    p.os_version = sh("sw_vers -productVersion 2>/dev/null");
    p.os_build   = sh("sw_vers -buildVersion 2>/dev/null");
    {
        std::string sysname = sh("uname -s 2>/dev/null");
        std::string rel     = sh("uname -r 2>/dev/null");
        p.kernel = trim(sysname + " " + rel);
    }

    // Apple developer tools
    if (!which("xcodebuild").empty()) {
        std::string xv = sh("xcodebuild -version 2>/dev/null");
        p.xcode_version = first_line(xv);
    }
    for (const char* dir : {"/Developer/SDKs",
                            "/Applications/Xcode.app/Contents/Developer/Platforms/"
                            "MacOSX.platform/Developer/SDKs"}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::string n = e.path().filename().string();
            if (ends_with(n, ".sdk")) p.sdks.push_back(n);
        }
    }
    std::sort(p.sdks.begin(), p.sdks.end());

    // MacPorts
    for (const char* prefix : {"/opt/local", "/usr/local"}) {
        if (exists(std::string(prefix) + "/bin/port")) {
            p.ports_prefix = prefix;
            break;
        }
    }
    if (!p.ports_prefix.empty()) {
        std::string pv = sh(p.ports_prefix + "/bin/port version 2>/dev/null");
        // "Version: 2.12.5"
        size_t c = pv.find(':');
        p.ports_version = trim(c == std::string::npos ? pv : pv.substr(c + 1));

        std::string listing = sh(p.ports_prefix +
                                 "/bin/port -q installed active 2>/dev/null",
                                 kSlowCmdTimeoutMs);
        for (const std::string& raw : split(listing, '\n')) {
            std::string line = trim(raw);
            if (line.empty()) continue;
            // "  name @version_rev (active)"
            size_t sp2 = line.find(" (active)");
            if (sp2 != std::string::npos) line = trim(line.substr(0, sp2));
            if (!line.empty()) p.ports.push_back(line);
        }
        p.ports_installed = static_cast<int>(p.ports.size());
    }

    // Toolchain
    p.compilers   = probe_group(kCompilers, sizeof(kCompilers) / sizeof(kCompilers[0]));
    p.languages   = probe_group(kLanguages, sizeof(kLanguages) / sizeof(kLanguages[0]));
    p.build_tools = probe_group(kBuildTools, sizeof(kBuildTools) / sizeof(kBuildTools[0]));

    for (const char* name : kCheckAbsent)
        if (which(name).empty()) p.absent.push_back(name);

    // Applications. Check the standard bundle locations for each.
    {
        std::vector<std::string> roots = {"/Applications", "/Applications/Utilities",
                                          "/Developer/Applications"};
        if (const char* home = std::getenv("HOME"); home && *home)
            roots.push_back(std::string(home) + "/Applications");

        for (const AppCheck& ac : kApps) {
            std::string found;
            for (const std::string& root : roots) {
                std::string candidate = root + "/" + ac.bundle + ".app";
                if (exists(candidate)) { found = candidate; break; }
            }
            if (found.empty()) {
                if (ac.suggestion && *ac.suggestion)
                    p.app_suggestions.push_back(ac.suggestion);
                continue;
            }
            std::string ver = bundle_version(found);
            p.apps.push_back(std::string(ac.label) + (ver.empty() ? "" : " " + ver));
        }
    }

    // Network reach. This is a live check, not a version guess: an old OpenSSL
    // will link fine and then fail every handshake, and the whole point is to
    // know which it is on *this* machine.
    p.curl_version = http::version_string();
    {
        size_t slash = p.curl_version.find(" / ");
        if (slash != std::string::npos)
            p.tls_backend = p.curl_version.substr(slash + 3);
    }
    {
        http::Response r = http::get("https://api.github.com/", {}, 25);
        if (r.error.empty() && r.status >= 200 && r.status < 400) {
            p.https_works = true;
        } else {
            p.https_works = false;
            p.https_note = r.error.empty()
                               ? ("HTTP " + std::to_string(r.status))
                               : r.error;
        }
    }

    // Assemble platform guidance from what we actually found.
    bool leopard_or_older = false;
    if (!p.os_version.empty()) {
        std::vector<std::string> parts = split(p.os_version, '.');
        if (parts.size() >= 2) {
            int major = std::atoi(parts[0].c_str());
            int minor = std::atoi(parts[1].c_str());
            leopard_or_older = (major == 10 && minor <= 5);
        }
    }

    if (leopard_or_older) {
        p.caveats.push_back(
            "Userland is the BSD/Leopard vintage: sed, awk, find, tar, cp, stat and "
            "friends do NOT accept GNU-only flags (no sed -i without an argument, no "
            "find -printf, no readlink -f, no grep -P). Prefer POSIX forms, or use "
            "the MacPorts g-prefixed tools if installed.");
        p.caveats.push_back(
            "There is no SIP, no notarization and no code signing requirement.");
    }
    if (p.big_endian) {
        p.caveats.push_back(
            "This is a BIG-ENDIAN machine. Code that assumes little-endian byte "
            "order, or that casts between byte buffers and integers without "
            "swapping, will misbehave. Test anything doing binary I/O.");
    }
    if (!p.ports_prefix.empty()) {
        p.caveats.push_back(
            "MacPorts is at " + p.ports_prefix + ". Headers are in " +
            p.ports_prefix + "/include and libraries in " + p.ports_prefix +
            "/lib; pass -I/-L for both when building against them.");
        // The single most expensive mistake a model can make on this hardware.
        p.caveats.push_back(
            "Installing a MacPorts package here means COMPILING IT FROM SOURCE. "
            "There are no binary archives for this architecture, and this is "
            "slow hardware: a large port (gcc, llvm, boost, qt) can take many "
            "hours to over a day. Never casually suggest `port install`. First "
            "check whether what you need is already present in the lists above, "
            "or use `port -q installed NAME` and `port deps NAME`. If an install "
            "really is required, say so explicitly, name the port, and warn that "
            "it is a long build -- do not start one as a side effect of another "
            "task.");
    } else {
        p.caveats.push_back(
            "No MacPorts installation was found. Only the stock Apple tools are "
            "available, which are very old. Assume no modern compiler, no modern "
            "TLS, and no package manager.");
    }
    if (p.https_works) {
        p.caveats.push_back(
            "Outbound HTTPS works from this machine (" + p.tls_backend +
            "), so fetching documentation and source archives is possible.");
    } else {
        p.caveats.push_back(
            "Outbound HTTPS is NOT working from this machine" +
            (p.https_note.empty() ? std::string() : " (" + p.https_note + ")") +
            ". Leopard's own curl and OpenSSL are far too old for today's TLS. "
            "Do not suggest downloading anything; work with what is on disk, or "
            "have the user fetch files from another machine.");
    }
    {
        bool has_node = true;
        for (const std::string& a : p.absent) if (a == "node") has_node = false;
        if (!has_node) {
            std::string msg =
                "There is no Node.js, npm, or any JavaScript package manager on "
                "this machine, and no port is available for it. Do not propose "
                "solutions that need them.";
            for (const ToolEntry& l : p.languages)
                if (l.name == "qjs")
                    msg += " QuickJS (qjs) is available for small scripts, but it "
                           "has no Node compatibility layer.";
            p.caveats.push_back(msg);
        }
    }
    // Surface any compiler-specific notes as caveats too, so a Brief render
    // still carries the ones that actually prevent a successful build.
    for (const ToolEntry& c : p.compilers)
        if (!c.note.empty() && c.name.find("clang") != std::string::npos)
            p.caveats.push_back(c.name + ": " + c.note);

    p.ok = true;
    return p;
}

Probe probe(bool force) {
    std::string path = cache_path();

    if (!force) {
        std::string text;
        if (read_file_text(path, &text, nullptr)) {
            try {
                Probe cached = Probe::from_json(json::parse(text));
                char host[256] = {0};
                gethostname(host, sizeof(host) - 1);
                // A cache from a different machine is worse than none, since the
                // whole point is host-specific accuracy.
                if (cached.ok && cached.schema == Probe().schema &&
                    cached.hostname == std::string(host)) {
                    return cached;
                }
            } catch (const std::exception&) {
                // fall through and re-probe
            }
        }
    }

    Probe p = do_probe();

    std::error_code ec;
    fs::path cp(path);
    if (cp.has_parent_path()) fs::create_directories(cp.parent_path(), ec);
    std::string err;
    if (!write_file_text(path, p.to_json().dump(2) + "\n", &err))
        log_line("envinfo: could not write cache: " + err);

    return p;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

size_t estimate_tokens(const std::string& text) {
    // Four characters per token is close enough for budgeting.
    return text.size() / 4 + 1;
}

namespace {

std::string host_line(const Probe& p) {
    std::string s;
    if (!p.model_name.empty()) s += p.model_name;
    else if (!p.machine_model.empty()) s += p.machine_model;
    else s += "unknown Mac";
    if (!p.machine_model.empty() && !p.model_name.empty())
        s += " (" + p.machine_model + ")";
    if (!p.cpu_count || !p.cpu_brand.empty()) {
        s += ", ";
        if (p.cpu_count > 1) s += std::to_string(p.cpu_count) + "x ";
        s += p.cpu_brand.empty() ? p.arch : p.cpu_brand;
        if (!p.cpu_speed.empty()) s += " " + p.cpu_speed;
    }
    if (p.memory_bytes) s += ", " + human_bytes(p.memory_bytes) + " RAM";
    return s;
}

std::string os_line(const Probe& p) {
    std::string s = p.os_name.empty() ? "Mac OS X" : p.os_name;
    if (!p.os_version.empty()) s += " " + p.os_version;
    if (!p.os_build.empty()) s += " (" + p.os_build + ")";
    if (!p.kernel.empty()) s += ", " + p.kernel;
    if (!p.arch.empty()) s += ", " + p.arch;
    s += p.big_endian ? ", big-endian" : ", little-endian";
    return s;
}

// The compiler a model should reach for first.
const ToolEntry* primary_compiler(const Probe& p) {
    for (const char* want : {"g++-mp-15", "gcc-mp-15", "g++-mp-14",
                             "gcc-mp-14", "clang", "gcc"})
        for (const ToolEntry& c : p.compilers)
            if (c.name == want) return &c;
    return p.compilers.empty() ? nullptr : &p.compilers.front();
}

void append_tools(std::string* out, const char* heading,
                  const std::vector<ToolEntry>& tools, bool with_notes) {
    if (tools.empty()) return;
    *out += heading;
    *out += "\n";
    for (const ToolEntry& t : tools) {
        *out += "  " + t.name;
        if (!t.version.empty()) *out += " -- " + elide(t.version, 90);
        *out += "\n";
        if (with_notes && !t.note.empty())
            for (const std::string& l : wrap_text(t.note, 76))
                *out += "      " + l + "\n";
    }
}

// Ports worth mentioning when we cannot afford the full list: toolchain,
// build systems, and the libraries people actually link against.
bool interesting_port(const std::string& entry) {
    static const char* keys[] = {
        "gcc", "clang", "llvm", "cmake", "autoconf", "automake", "libtool",
        "pkgconfig", "gmake", "ninja", "git", "python", "perl", "ruby",
        "openjdk", "quickjs", "openssl", "curl", "ncurses", "readline",
        "zlib", "libiconv", "gettext", "boost", "sqlite", "libxml", "libpng",
        "freetype", "fontconfig", "pcre", "icu", "ld64", "cctools",
        "legacy-support", "libgcc", "rsync", "openssh", "bash", "zsh",
    };
    std::string low = to_lower(entry);
    for (const char* k : keys)
        if (low.find(k) != std::string::npos) return true;
    return false;
}

} // namespace

std::string render(const Probe& p, Detail d) {
    if (!p.ok || d == Detail::None) return "";

    const ToolEntry* pc = primary_compiler(p);

    if (d == Detail::Minimal) {
        std::string s = "Host: " + host_line(p) + "; " + os_line(p) + ".";
        if (pc) s += " Primary compiler: " + pc->name + ".";
        bool no_node = false;
        for (const std::string& a : p.absent) if (a == "node") no_node = true;
        if (no_node) s += " No Node.js.";
        if (p.big_endian) s += " Big-endian.";
        return s;
    }

    std::string out = "## Build environment\n\n";
    out += "Machine: " + host_line(p) + "\n";
    out += "System:  " + os_line(p) + "\n";
    if (!p.hostname.empty()) out += "Host:    " + p.hostname + "\n";

    if (d == Detail::Brief) {
        if (pc) {
            out += "Compiler: " + pc->name;
            if (!pc->version.empty()) out += " (" + elide(pc->version, 70) + ")";
            out += "\n";
        }
        if (!p.ports_prefix.empty())
            out += "MacPorts: " + p.ports_prefix + ", " +
                   std::to_string(p.ports_installed) + " ports installed\n";
        if (!p.caveats.empty()) {
            out += "\nImportant:\n";
            for (const std::string& c : p.caveats)
                for (const std::string& l : wrap_text("- " + c, 78))
                    out += l + "\n";
        }
        return out;
    }

    // Standard and Full
    if (!p.xcode_version.empty()) out += "Xcode:   " + p.xcode_version + "\n";
    if (!p.sdks.empty())          out += "SDKs:    " + join(p.sdks, ", ") + "\n";
    if (!p.ports_prefix.empty()) {
        out += "MacPorts: " + p.ports_prefix;
        if (!p.ports_version.empty()) out += " (port " + p.ports_version + ")";
        out += ", " + std::to_string(p.ports_installed) + " active ports\n";
    }
    out += "Network: ";
    out += p.https_works ? "outbound HTTPS works" : "outbound HTTPS FAILS";
    if (!p.tls_backend.empty()) out += " (" + p.tls_backend + ")";
    if (!p.https_works && !p.https_note.empty()) out += " -- " + p.https_note;
    out += "\n\n";

    append_tools(&out, "Compilers:", p.compilers, true);
    if (!p.languages.empty())   { out += "\n"; append_tools(&out, "Languages and runtimes:", p.languages, true); }
    if (!p.build_tools.empty()) { out += "\n"; append_tools(&out, "Build tools:", p.build_tools, d == Detail::Full); }

    if (!p.absent.empty()) {
        out += "\nNot installed: " + join(p.absent, ", ") + "\n";
    }
    if (!p.apps.empty()) {
        out += "Applications: " + join(p.apps, ", ") + "\n";
    }
    if (!p.app_suggestions.empty()) {
        for (const std::string& s : p.app_suggestions)
            for (const std::string& l : wrap_text("- " + s, 78))
                out += l + "\n";
    }

    if (!p.ports.empty()) {
        std::vector<std::string> show;
        if (d == Detail::Full) {
            show = p.ports;
        } else {
            for (const std::string& e : p.ports)
                if (interesting_port(e)) show.push_back(e);
        }
        if (!show.empty()) {
            out += "\n";
            out += (d == Detail::Full) ? "Installed ports (all):\n"
                                       : "Installed ports (toolchain and common libraries):\n";
            // Pack several per line; one per line is a lot of tokens.
            std::string line = " ";
            for (const std::string& e : show) {
                if (line.size() + e.size() + 2 > 78) {
                    out += line + "\n";
                    line = " ";
                }
                line += " " + e;
            }
            if (trim(line).size()) out += line + "\n";
            if (d != Detail::Full && show.size() < p.ports.size())
                out += "  (" + std::to_string(p.ports.size() - show.size()) +
                       " more; run `port -q installed active` for the full list)\n";
        }
    }

    if (!p.caveats.empty()) {
        out += "\nPlatform notes -- these prevent build failures, read them:\n";
        for (const std::string& c : p.caveats) {
            std::vector<std::string> ls = wrap_text("- " + c, 78);
            for (size_t i = 0; i < ls.size(); i++)
                out += (i == 0 ? "" : "  ") + ls[i] + "\n";
        }
    }

    return out;
}

Detail choose_detail(const Probe& p, int64_t context_tokens, double max_fraction) {
    if (!p.ok) return Detail::None;

    // Start from the window size. Big frontier models can afford the whole
    // picture; small ones need the few facts that prevent failures.
    Detail d;
    if (context_tokens <= 0)            d = Detail::Standard;   // unknown, be moderate
    else if (context_tokens >= 400000)  d = Detail::Full;
    else if (context_tokens >= 120000)  d = Detail::Standard;
    else if (context_tokens >= 32000)   d = Detail::Brief;
    else if (context_tokens >= 8000)    d = Detail::Minimal;
    else                                d = Detail::None;

    if (context_tokens <= 0) return d;

    // Then make sure it actually fits the budget, stepping down if not.
    const size_t budget = static_cast<size_t>(
        static_cast<double>(context_tokens) * max_fraction);
    for (;;) {
        if (d == Detail::None) return d;
        if (estimate_tokens(render(p, d)) <= budget) return d;
        switch (d) {
            case Detail::Full:     d = Detail::Standard; break;
            case Detail::Standard: d = Detail::Brief;    break;
            case Detail::Brief:    d = Detail::Minimal;  break;
            case Detail::Minimal:  return Detail::Minimal;  // always worth this much
            case Detail::None:     return d;
        }
    }
}

} // namespace ppcode::envinfo

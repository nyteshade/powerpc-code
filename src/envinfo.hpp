// envinfo.hpp -- what machine is this, and what can it build?
//
// Models waste a lot of turns guessing at an unfamiliar host: reaching for GNU
// flags that the BSD userland does not have, calling a compiler that is too old
// for the standard they want, or suggesting a package manager that is not here.
// Probing the machine once and stating the answers up front removes most of
// that churn.
//
// The probe shells out to sysctl/sw_vers/port and is slow enough on a G5 that
// results are cached on disk between runs.
#pragma once

#include "common.hpp"

namespace ppcode::envinfo {

// How much of the probe to put in front of the model. Chosen from the model's
// context window unless the job overrides it.
enum class Detail {
    None,       // say nothing
    Minimal,    // one line: machine, OS, primary compiler
    Brief,      // a short paragraph plus the critical caveats
    Standard,   // host, compilers, languages, build tools, port summary
    Full,       // everything, including the full installed-port list
};

Detail detail_from_string(const std::string& s, bool* ok);
std::string detail_to_string(Detail d);

struct ToolEntry {
    std::string name;       // as invoked, e.g. "g++-mp-15"
    std::string path;
    std::string version;    // first useful line of --version
    std::string note;       // platform caveat worth stating up front
};

struct Probe {
    // Identity and hardware
    std::string hostname;
    std::string machine_model;      // e.g. PowerMac11,2
    std::string model_name;         // e.g. Power Mac G5
    std::string cpu_brand;          // e.g. PowerPC G5 (1.1)
    std::string cpu_speed;
    std::string arch;               // e.g. ppc
    int cpu_count = 0;
    uint64_t memory_bytes = 0;
    bool big_endian = false;

    // Operating system
    std::string os_name;            // Mac OS X Server
    std::string os_version;         // 10.5.8
    std::string os_build;           // 9L34
    std::string kernel;             // Darwin 9.8.0

    // Apple developer tools
    std::string xcode_version;
    std::vector<std::string> sdks;

    // Notable installed applications, and recommendations for absent ones. The
    // stock browser on 10.5 cannot reach most of today's web, so whether a
    // modern one is installed materially changes what advice is useful.
    std::vector<std::string> apps;           // "PowerFox 26.1.0"
    std::vector<std::string> app_suggestions;

    // MacPorts
    std::string ports_prefix;
    std::string ports_version;
    int ports_installed = 0;
    std::vector<std::string> ports;         // full active list, "name @version"

    // Toolchain
    std::vector<ToolEntry> compilers;
    std::vector<ToolEntry> languages;
    std::vector<ToolEntry> build_tools;
    std::vector<std::string> absent;        // notable things that are NOT here

    // Network reach. Leopard's own curl/OpenSSL cannot negotiate with most of
    // today's internet, so whether this machine can fetch anything at all
    // depends entirely on which MacPorts are present -- and that varies per
    // machine. Probed once, because the answer decides whether web tools and
    // "just download X" suggestions are viable.
    std::string curl_version;               // libcurl linked into ppcode
    std::string tls_backend;                // e.g. OpenSSL/3.6.3
    bool https_works = false;               // a real request to a known host succeeded
    std::string https_note;                 // why it failed, when it did

    // Free-form platform guidance assembled from what was found.
    std::vector<std::string> caveats;

    std::string probed_at;                  // ISO-ish local timestamp
    int schema = 2;

    bool ok = false;                        // false if the probe never ran

    json to_json() const;
    static Probe from_json(const json& j);
};

// Probe the machine. Uses the on-disk cache unless `force` is set or the cache
// is stale/for a different host. Never throws.
Probe probe(bool force = false);

// Where the cache lives ($XDG_CACHE_HOME/ppcode/envprobe.json).
std::string cache_path();

// Render the probe as a system-message section.
std::string render(const Probe& p, Detail d);

// Rough token estimate, for deciding how much to include.
size_t estimate_tokens(const std::string& text);

// Pick a detail level for a context window, then step it down if the rendered
// text would take more than `max_fraction` of that window.
Detail choose_detail(const Probe& p, int64_t context_tokens,
                     double max_fraction = 0.04);

} // namespace ppcode::envinfo

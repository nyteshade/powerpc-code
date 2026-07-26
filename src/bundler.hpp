// bundler.hpp -- assembling .app bundles and making them self-contained.
//
// Software built here links against MacPorts, which means it runs on this
// machine and nowhere else: every dylib reference is an absolute /opt/local
// path that will not exist on another Mac. Shipping anything therefore means
// copying the libraries into the bundle and rewriting their install names.
//
// Doing that by hand is where it usually goes wrong, because the dependencies
// are transitive: rewriting the executable's references is easy, and forgetting
// that libssl also points at libcrypto is what produces a bundle that launches
// on the build machine and dies everywhere else. This walks the whole graph and
// verifies afterwards that nothing still points into the MacPorts prefix.
#pragma once

#include "common.hpp"
#include "tools.hpp"

namespace ppcode::bundle {

struct DylibRef {
    std::string path;        // as recorded in the Mach-O
    bool is_system = false;  // /usr/lib or /System, which must NOT be bundled
};

// Read the load commands of a Mach-O with otool.
std::vector<DylibRef> dependencies(const std::string& binary);

// The install name a dylib reports for itself.
std::string install_name(const std::string& dylib);

struct RelocateResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> copied;       // libraries brought into the bundle
    std::vector<std::string> rewritten;    // binaries whose paths were changed
    std::vector<std::string> leaks;        // remaining non-system absolute paths
    std::string report;
};

// Copy every non-system dependency of `binary`, transitively, into
// `dest_dir`, and rewrite all references to @loader_path or @executable_path.
// `prefixes` limits which absolute paths are considered relocatable; anything
// under /usr/lib or /System is always left alone.
RelocateResult relocate(const std::string& binary, const std::string& dest_dir,
                        const std::string& loader_prefix,
                        const std::vector<std::string>& prefixes,
                        bool dry_run);

struct BundleSpec {
    std::string app_path;        // .../Name.app
    std::string executable;      // path to the built binary
    std::string name;            // CFBundleName
    std::string identifier;      // CFBundleIdentifier
    std::string version = "1.0";
    std::string icon;            // optional .icns to copy in
    std::vector<std::string> resources;
    std::string min_system = "10.5";
    bool relocate_libs = true;
};

struct BundleResult {
    bool ok = false;
    std::string error;
    std::string report;
};

BundleResult make_bundle(const BundleSpec& spec);

void add_tools(ToolRegistry& registry);

} // namespace ppcode::bundle

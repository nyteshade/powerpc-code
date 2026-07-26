// version.hpp -- the one place the version number is read from.
//
// The number itself lives in the VERSION file at the root of the tree. The
// Makefile reads that file and passes it in as PPCODE_VERSION, so the build,
// the --version flag, the application bundle's Info.plist and the release tag
// cannot drift apart. Nothing here should ever be edited to bump a version;
// edit VERSION, or run scripts/release.sh.
#pragma once

#include <string>

// Falls back only when compiled outside the Makefile, e.g. by an IDE indexer.
#ifndef PPCODE_VERSION
#define PPCODE_VERSION "0.0.0-dev"
#endif

// Set by the Makefile from .build-rev, which deploy.sh writes on the machine
// that actually has the git checkout. Empty for a build from a plain tarball.
#ifndef PPCODE_BUILD_REV
#define PPCODE_BUILD_REV ""
#endif

namespace ppcode {

// Semantic version, e.g. "0.2.0".
inline std::string version() { return PPCODE_VERSION; }

// Short git revision, or empty when it is not known.
inline std::string build_rev() { return PPCODE_BUILD_REV; }

// "0.2.0" or "0.2.0+g1a2b3c4" -- what --version prints and what goes in the
// bundle's CFBundleVersion.
inline std::string version_full() {
    std::string v = version();
    std::string r = build_rev();
    if (!r.empty()) v += "+g" + r;

    return v;
}

} // namespace ppcode

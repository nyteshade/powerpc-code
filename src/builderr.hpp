// builderr.hpp -- running builds and turning their output into structured
// diagnostics.
//
// A failing build here emits hundreds of lines, and the model pays for every one
// of them on every subsequent round. Worse, the useful information is usually
// three lines buried in the middle: the first error, and the line the compiler
// was pointing at. Parsing that out means the model reads a summary instead of a
// log, which saves both tokens and rounds -- and rounds are minutes on this
// hardware.
#pragma once

#include "common.hpp"
#include "tools.hpp"

namespace ppcode::builderr {

struct Diagnostic {
    std::string file;
    int line = 0;
    int column = 0;
    std::string severity;    // error | warning | note
    std::string message;
    std::string context;     // the source line the compiler echoed, if any

    std::string format() const;
};

struct Report {
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> link_errors;   // undefined symbols and friends
    std::vector<std::string> make_failures; // "make: *** [foo] Error 1"
    bool succeeded = false;                 // saw an explicit success marker
    bool failed = false;                    // saw an explicit failure marker
    int exit_code = -1;

    int error_count() const;
    int warning_count() const;
    // A compact human-readable summary, ordered errors first.
    std::string summarise(size_t max_diagnostics = 25) const;
};

// Parse compiler, linker and make output. Handles the GCC/clang
// file:line:col: severity: message form used by every compiler on this machine,
// Apple's linker messages, and xcodebuild's success and failure markers.
Report parse(const std::string& output);

void add_tools(ToolRegistry& registry);

} // namespace ppcode::builderr

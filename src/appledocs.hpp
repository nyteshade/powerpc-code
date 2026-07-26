// appledocs.hpp -- searching Apple's local developer documentation.
//
// Xcode 3 installs its reference library under /Developer/Documentation as
// docsets, each carrying a SQLite index (docSet.dsidx) of every documented API:
// on this machine that is roughly 61,000 tokens with declarations, abstracts,
// owning class and framework, and deprecation notes.
//
// This matters more here than a docs tool normally would. A model's training
// data is dominated by modern macOS, so it will confidently reach for APIs that
// do not exist on 10.5 -- and the failure shows up as a compile error several
// minutes later on slow hardware. Consulting the documentation that shipped with
// *this* OS is authoritative, costs no network, and settles the question before
// anything is built.
#pragma once

#include "common.hpp"
#include "tools.hpp"

namespace ppcode::appledocs {

struct Entry {
    std::string name;
    std::string type;          // human-readable: class, instance method, ...
    std::string container;     // owning class or protocol
    std::string framework;
    std::string declaration;   // plain text, HTML stripped
    std::string abstract;
    std::string deprecation;
    std::string docset;
};

// Docsets found under /Developer/Documentation (and the Xcode-inside-app
// location, for completeness).
std::vector<std::string> docset_indexes();

// True if any documentation is installed.
bool available();

struct Query {
    std::string name;
    std::string type_filter;       // "class", "method", "function", ...
    std::string framework_filter;
    int limit = 12;
};

// Search the indexes. Tries exact match, then prefix, then substring, so a
// half-remembered name still finds something.
std::vector<Entry> search(const Query& q, std::string* error);

// Grep the installed framework headers for a declaration. Used when the docset
// has nothing, which happens for newer or undocumented symbols.
std::vector<std::string> search_headers(const std::string& name, int limit);

void add_tools(ToolRegistry& registry);

} // namespace ppcode::appledocs

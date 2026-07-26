// plist.hpp -- the old-style NeXT/OpenStep ASCII property list format.
//
// This is the format Xcode 3 uses for project.pbxproj. It is not XML and not
// binary: it is plain text that looks like
//
//   // !$*UTF8*$!
//   {
//       archiveVersion = 1;
//       objects = {
//           ABC123 /* main.m */ = {
//               isa = PBXFileReference;
//               path = main.m;
//               name = "quoted when it needs to be";
//           };
//       };
//       list = ( one, two, );
//   }
//
// Being text means a project file can be edited reliably in place, which is what
// makes Xcode tooling possible here at all. Key order and the /* comment */
// annotations are preserved so that a rewritten file stays readable and diffs
// against the original stay small.
#pragma once

#include "common.hpp"

#include <memory>

namespace ppcode::plist {

class Value;
using ValuePtr = std::shared_ptr<Value>;

class Value {
public:
    enum class Type { String, Dict, Array };

    Type type = Type::String;

    // String
    std::string str;

    // Dict: insertion-ordered so rewriting does not shuffle the file.
    std::vector<std::pair<std::string, ValuePtr>> entries;

    // Array
    std::vector<ValuePtr> items;

    // The /* ... */ annotation Xcode writes after a key or value, kept so the
    // file stays human-readable after a rewrite.
    std::string comment;

    static ValuePtr make_string(const std::string& s) {
        auto v = std::make_shared<Value>();
        v->type = Type::String;
        v->str = s;
        return v;
    }
    static ValuePtr make_dict() {
        auto v = std::make_shared<Value>();
        v->type = Type::Dict;
        return v;
    }
    static ValuePtr make_array() {
        auto v = std::make_shared<Value>();
        v->type = Type::Array;
        return v;
    }

    bool is_string() const { return type == Type::String; }
    bool is_dict() const { return type == Type::Dict; }
    bool is_array() const { return type == Type::Array; }

    // Dict access. Returns null when absent or when this is not a dict.
    ValuePtr get(const std::string& key) const;
    std::string get_string(const std::string& key,
                           const std::string& def = "") const;
    void set(const std::string& key, ValuePtr value);
    bool erase(const std::string& key);
    bool has(const std::string& key) const { return get(key) != nullptr; }

    std::vector<std::string> keys() const;
};

// Parse an old-style plist. Returns null with `error` set on failure.
ValuePtr parse(const std::string& text, std::string* error);

// Serialise in Xcode's own layout: two-space indent, one entry per line, and the
// `objects` section grouped by isa with /* Begin ... */ banners.
std::string serialize(const ValuePtr& root, bool pbxproj_style);

// True if `s` can be written without quotes in this format.
bool needs_quoting(const std::string& s);
std::string quote(const std::string& s);

} // namespace ppcode::plist

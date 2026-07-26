// xib.hpp -- reading and editing Interface Builder documents.
//
// A .xib is an XML archive of a Cocoa object graph: <object class="NSWindow"
// id="123"> holding typed keys, with <reference ref="..."/> for edges. Xcode 3
// writes version 7.x of the format.
//
// Until now ppcode could write Cocoa code but was blind to the interface, which
// is most of a Mac application. Reading the object graph is the larger half of
// closing that gap: knowing which windows and controls exist, what classes the
// nib expects, and which outlets and actions are declared but not connected is
// what lets the model write code that actually matches the nib.
//
// Editing is deliberately limited to the operations that can be performed
// safely. A malformed nib still opens and then misbehaves, so anything this
// cannot do correctly it declines to do rather than guessing.
#pragma once

#include "common.hpp"
#include "tools.hpp"
#include "xml.hpp"

namespace ppcode::xib {

struct Connection {
    std::string kind;       // outlet | action | binding
    std::string label;      // outlet or action name
    std::string source;     // id
    std::string destination;// id
    std::string source_class;
    std::string dest_class;
};

struct ClassDescription {
    std::string name;
    std::string superclass;
    std::vector<std::pair<std::string, std::string>> outlets;  // name -> type
    std::vector<std::string> actions;
    std::string source_file;    // where IB thinks the header is
};

struct ObjectNode {
    std::string id;
    std::string cls;
    std::string title;      // NSTitle, NSFrame or whatever identifies it
    int depth = 0;
    std::vector<std::string> children;
};

class Document {
public:
    bool load(const std::string& path, std::string* error);
    bool save(std::string* error) const;

    const std::string& path() const { return path_; }
    std::string format_version() const;
    std::string system_target() const;   // e.g. 1050 for 10.5

    // Everything in IBDocument.RootObjects, flattened with nesting depth.
    std::vector<ObjectNode> objects() const;
    std::vector<Connection> connections() const;
    std::vector<ClassDescription> classes() const;

    // Human-readable summary for the info tool.
    std::string describe(bool include_all_objects) const;

    // Declare a custom class, so Interface Builder offers its outlets and
    // actions in the inspector. This is the edit that is both safe and most
    // often needed: the nib has to know about a controller before anything can
    // be wired to it.
    bool add_class(const ClassDescription& desc, std::string* error);

    // Rename or remove a declared outlet/action on an existing class.
    bool remove_class(const std::string& name, std::string* error);

private:
    xml::Document doc_;
    std::string path_;

    xml::NodePtr data() const;
    xml::NodePtr keyed(const xml::NodePtr& parent, const std::string& key) const;
    xml::NodePtr root_objects() const;
    xml::NodePtr flattened_objects() const;
};

void add_tools(ToolRegistry& registry);

} // namespace ppcode::xib

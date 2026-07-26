// xml.hpp -- a small XML DOM, enough for Interface Builder files and XML plists.
//
// There is no XML library available here that is worth linking for this, and the
// documents in question are machine-written and well-formed. So this handles the
// subset those files actually use: elements, attributes, text, comments, CDATA,
// the standard entities and numeric character references. It preserves attribute
// order and round-trips a document unchanged, which matters because a .xib that
// Interface Builder cannot reopen is worse than no edit at all.
//
// Not supported, and rejected rather than half-read: DTDs beyond the doctype
// line, namespaces as anything other than part of the tag name, and entity
// definitions.
#pragma once

#include "common.hpp"

#include <memory>

namespace ppcode::xml {

class Node;
using NodePtr = std::shared_ptr<Node>;

class Node {
public:
    enum class Type { Element, Text, Comment, CData };

    Type type = Type::Element;
    std::string name;                                        // Element
    std::string text;                                        // Text/Comment/CData
    std::vector<std::pair<std::string, std::string>> attrs;   // ordered
    std::vector<NodePtr> children;
    Node* parent = nullptr;                                   // not owning

    static NodePtr element(const std::string& name);
    static NodePtr text_node(const std::string& s);

    bool is_element() const { return type == Type::Element; }

    // Attribute access. `has` distinguishes an absent attribute from an empty one.
    std::string attr(const std::string& key, const std::string& def = "") const;
    bool has_attr(const std::string& key) const;
    void set_attr(const std::string& key, const std::string& value);
    bool remove_attr(const std::string& key);

    // Direct children with this tag name.
    std::vector<NodePtr> find_children(const std::string& name) const;
    NodePtr first_child(const std::string& name) const;

    // Depth-first search over the whole subtree.
    std::vector<NodePtr> find_all(const std::string& name) const;
    // ... restricted to elements carrying attr `key` == `value`.
    std::vector<NodePtr> find_all_with_attr(const std::string& name,
                                            const std::string& key,
                                            const std::string& value) const;

    void append(NodePtr child);
    bool remove_child(const NodePtr& child);

    // Concatenated text of this element's direct text children.
    std::string inner_text() const;
    void set_inner_text(const std::string& s);
};

struct Document {
    std::string declaration;    // the <?xml ... ?> line, verbatim
    std::string doctype;        // the <!DOCTYPE ...> line, verbatim
    NodePtr root;
};

// Parse a document. Returns false with a line-numbered message on failure.
bool parse(const std::string& text, Document* out, std::string* error);

// Serialise. `indent` re-indents with tabs the way Interface Builder writes;
// pass false to keep everything on as few lines as the content allows.
std::string serialize(const Document& doc, bool indent = true);

std::string escape(const std::string& s, bool in_attribute = false);
std::string unescape(const std::string& s);

} // namespace ppcode::xml

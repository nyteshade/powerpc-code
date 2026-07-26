#include "xml.hpp"

#include "utf8.hpp"

#include <cctype>
#include <cstdlib>

namespace ppcode::xml {

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

NodePtr Node::element(const std::string& name) {
    auto n = std::make_shared<Node>();
    n->type = Type::Element;
    n->name = name;
    return n;
}

NodePtr Node::text_node(const std::string& s) {
    auto n = std::make_shared<Node>();
    n->type = Type::Text;
    n->text = s;
    return n;
}

std::string Node::attr(const std::string& key, const std::string& def) const {
    for (const auto& [k, v] : attrs)
        if (k == key) return v;
    return def;
}

bool Node::has_attr(const std::string& key) const {
    for (const auto& [k, v] : attrs)
        if (k == key) return true;
    return false;
}

void Node::set_attr(const std::string& key, const std::string& value) {
    for (auto& [k, v] : attrs) {
        if (k == key) { v = value; return; }
    }
    attrs.emplace_back(key, value);
}

bool Node::remove_attr(const std::string& key) {
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        if (it->first == key) { attrs.erase(it); return true; }
    }
    return false;
}

std::vector<NodePtr> Node::find_children(const std::string& n) const {
    std::vector<NodePtr> out;
    for (const NodePtr& c : children)
        if (c->is_element() && c->name == n) out.push_back(c);
    return out;
}

NodePtr Node::first_child(const std::string& n) const {
    for (const NodePtr& c : children)
        if (c->is_element() && c->name == n) return c;
    return nullptr;
}

std::vector<NodePtr> Node::find_all(const std::string& n) const {
    std::vector<NodePtr> out;
    for (const NodePtr& c : children) {
        if (!c->is_element()) continue;
        if (c->name == n) out.push_back(c);
        for (const NodePtr& d : c->find_all(n)) out.push_back(d);
    }
    return out;
}

std::vector<NodePtr> Node::find_all_with_attr(const std::string& n,
                                              const std::string& key,
                                              const std::string& value) const {
    std::vector<NodePtr> out;
    for (const NodePtr& c : find_all(n))
        if (c->attr(key) == value) out.push_back(c);
    return out;
}

void Node::append(NodePtr child) {
    child->parent = this;
    children.push_back(std::move(child));
}

bool Node::remove_child(const NodePtr& child) {
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (*it == child) { children.erase(it); return true; }
    }
    return false;
}

std::string Node::inner_text() const {
    std::string out;
    for (const NodePtr& c : children)
        if (c->type == Type::Text || c->type == Type::CData) out += c->text;
    return out;
}

void Node::set_inner_text(const std::string& s) {
    children.clear();
    if (!s.empty()) append(Node::text_node(s));
}

// ---------------------------------------------------------------------------
// Escaping
// ---------------------------------------------------------------------------

std::string escape(const std::string& s, bool in_attribute) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += in_attribute ? "&quot;" : "\""; break;
            case '\'': out += in_attribute ? "&apos;" : "'";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += s[i]; continue; }
        std::string body = s.substr(i + 1, semi - i - 1);

        if (!body.empty() && body[0] == '#') {
            uint32_t cp = 0;
            if (body.size() > 2 && (body[1] == 'x' || body[1] == 'X'))
                cp = static_cast<uint32_t>(std::strtoul(body.c_str() + 2, nullptr, 16));
            else
                cp = static_cast<uint32_t>(std::strtoul(body.c_str() + 1, nullptr, 10));
            if (cp > 0 && cp <= 0x10FFFF) {
                out += utf8::encode(cp);
                i = semi;
                continue;
            }
            out += s[i];
            continue;
        }
        if      (body == "amp")  { out += '&';  i = semi; }
        else if (body == "lt")   { out += '<';  i = semi; }
        else if (body == "gt")   { out += '>';  i = semi; }
        else if (body == "quot") { out += '"';  i = semi; }
        else if (body == "apos") { out += '\''; i = semi; }
        else out += s[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

class Parser {
public:
    Parser(const std::string& s) : s_(s) {}

    bool run(Document* out, std::string* error) {
        skip_space();
        // <?xml ... ?>
        if (starts("<?")) {
            size_t end = s_.find("?>", i_);
            if (end == std::string::npos) { fail("unterminated <? ... ?>"); return done(error); }
            out->declaration = s_.substr(i_, end + 2 - i_);
            i_ = end + 2;
            skip_space();
        }
        // <!DOCTYPE ...>
        while (starts("<!") && !starts("<!--")) {
            size_t end = s_.find('>', i_);
            if (end == std::string::npos) { fail("unterminated <! ... >"); return done(error); }
            out->doctype = s_.substr(i_, end + 1 - i_);
            i_ = end + 1;
            skip_space();
        }
        // Leading comments are dropped; nothing here needs them preserved.
        while (starts("<!--")) {
            size_t end = s_.find("-->", i_);
            if (end == std::string::npos) { fail("unterminated comment"); return done(error); }
            i_ = end + 3;
            skip_space();
        }

        if (!starts("<")) { fail("no root element"); return done(error); }
        out->root = parse_element();
        if (!err_.empty()) return done(error);
        if (!out->root) { fail("no root element"); return done(error); }
        return true;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    std::string err_;

    bool done(std::string* error) {
        if (error) *error = err_;
        return err_.empty();
    }
    void fail(const std::string& m) {
        if (!err_.empty()) return;
        // A line number is far more useful than a byte offset in a 130 KB xib.
        int line = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); k++)
            if (s_[k] == '\n') line++;
        err_ = "line " + std::to_string(line) + ": " + m;
    }
    bool starts(const char* lit) const { return s_.compare(i_, std::strlen(lit), lit) == 0; }
    void skip_space() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) i_++;
    }
    bool name_char(char c) const {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
               c == '.' || c == ':';
    }
    std::string read_name() {
        size_t start = i_;
        while (i_ < s_.size() && name_char(s_[i_])) i_++;
        return s_.substr(start, i_ - start);
    }

    NodePtr parse_element() {
        if (!starts("<")) { fail("expected '<'"); return nullptr; }
        i_++;
        std::string name = read_name();
        if (name.empty()) { fail("expected an element name"); return nullptr; }

        NodePtr node = Node::element(name);

        for (;;) {
            skip_space();
            if (i_ >= s_.size()) { fail("unterminated start tag"); return node; }
            if (starts("/>")) { i_ += 2; return node; }     // empty element
            if (starts(">"))  { i_ += 1; break; }

            std::string key = read_name();
            if (key.empty()) { fail("bad attribute name"); i_++; continue; }
            skip_space();
            if (i_ >= s_.size() || s_[i_] != '=') { fail("expected '=' after " + key); return node; }
            i_++;
            skip_space();
            if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) {
                fail("expected a quoted attribute value for " + key);
                return node;
            }
            char q = s_[i_++];
            size_t start = i_;
            while (i_ < s_.size() && s_[i_] != q) i_++;
            std::string value = unescape(s_.substr(start, i_ - start));
            if (i_ < s_.size()) i_++;   // closing quote
            node->set_attr(key, value);
        }

        // Content until the matching close tag.
        for (;;) {
            if (i_ >= s_.size()) { fail("unterminated element <" + name + ">"); return node; }

            if (starts("</")) {
                i_ += 2;
                std::string close = read_name();
                skip_space();
                if (i_ < s_.size() && s_[i_] == '>') i_++;
                if (close != name) fail("</" + close + "> closes <" + name + ">");
                return node;
            }
            if (starts("<!--")) {
                size_t end = s_.find("-->", i_);
                if (end == std::string::npos) { fail("unterminated comment"); return node; }
                auto c = std::make_shared<Node>();
                c->type = Node::Type::Comment;
                c->text = s_.substr(i_ + 4, end - i_ - 4);
                node->append(c);
                i_ = end + 3;
                continue;
            }
            if (starts("<![CDATA[")) {
                size_t end = s_.find("]]>", i_);
                if (end == std::string::npos) { fail("unterminated CDATA"); return node; }
                auto c = std::make_shared<Node>();
                c->type = Node::Type::CData;
                c->text = s_.substr(i_ + 9, end - i_ - 9);
                node->append(c);
                i_ = end + 3;
                continue;
            }
            if (starts("<")) {
                NodePtr child = parse_element();
                if (!child) return node;
                node->append(child);
                if (!err_.empty()) return node;
                continue;
            }
            // Text run.
            size_t start = i_;
            while (i_ < s_.size() && s_[i_] != '<') i_++;
            std::string raw = s_.substr(start, i_ - start);
            // Whitespace between elements is formatting, not content.
            if (trim(raw).empty()) continue;
            node->append(Node::text_node(unescape(raw)));
        }
    }
};

void write(const NodePtr& n, std::string* out, int depth, bool indent) {
    std::string pad = indent ? std::string(static_cast<size_t>(depth), '\t') : "";

    switch (n->type) {
        case Node::Type::Text:
            *out += escape(n->text);
            return;
        case Node::Type::Comment:
            if (indent) *out += pad;
            *out += "<!--" + n->text + "-->";
            if (indent) *out += "\n";
            return;
        case Node::Type::CData:
            if (indent) *out += pad;
            *out += "<![CDATA[" + n->text + "]]>";
            if (indent) *out += "\n";
            return;
        case Node::Type::Element:
            break;
    }

    if (indent) *out += pad;
    *out += "<" + n->name;
    for (const auto& [k, v] : n->attrs)
        *out += " " + k + "=\"" + escape(v, true) + "\"";

    if (n->children.empty()) {
        *out += "/>";
        if (indent) *out += "\n";
        return;
    }

    // An element whose content is only text stays on one line, which is how
    // Interface Builder writes <string key="...">value</string>.
    bool text_only = true;
    for (const NodePtr& c : n->children)
        if (c->is_element()) { text_only = false; break; }

    *out += ">";
    if (text_only) {
        for (const NodePtr& c : n->children) write(c, out, 0, false);
        *out += "</" + n->name + ">";
        if (indent) *out += "\n";
        return;
    }

    if (indent) *out += "\n";
    for (const NodePtr& c : n->children) write(c, out, depth + 1, indent);
    if (indent) *out += pad;
    *out += "</" + n->name + ">";
    if (indent) *out += "\n";
}

} // namespace

bool parse(const std::string& text, Document* out, std::string* error) {
    Parser p(text);
    return p.run(out, error);
}

std::string serialize(const Document& doc, bool indent) {
    std::string out;
    if (!doc.declaration.empty()) {
        out += doc.declaration;
        out += "\n";
    }
    if (!doc.doctype.empty()) {
        out += doc.doctype;
        out += "\n";
    }
    if (doc.root) write(doc.root, &out, 0, indent);
    return out;
}

} // namespace ppcode::xml

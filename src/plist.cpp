#include "plist.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace ppcode::plist {

// ---------------------------------------------------------------------------
// Value accessors
// ---------------------------------------------------------------------------

ValuePtr Value::get(const std::string& key) const {
    if (type != Type::Dict) return nullptr;
    for (const auto& [k, v] : entries)
        if (k == key) return v;
    return nullptr;
}

std::string Value::get_string(const std::string& key, const std::string& def) const {
    ValuePtr v = get(key);
    if (!v || !v->is_string()) return def;
    return v->str;
}

void Value::set(const std::string& key, ValuePtr value) {
    if (type != Type::Dict) return;
    for (auto& [k, v] : entries) {
        if (k == key) { v = std::move(value); return; }
    }
    entries.emplace_back(key, std::move(value));
}

bool Value::erase(const std::string& key) {
    if (type != Type::Dict) return false;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->first == key) { entries.erase(it); return true; }
    }
    return false;
}

std::vector<std::string> Value::keys() const {
    std::vector<std::string> out;
    if (type != Type::Dict) return out;
    for (const auto& [k, v] : entries) out.push_back(k);
    return out;
}

// ---------------------------------------------------------------------------
// Quoting
// ---------------------------------------------------------------------------

bool needs_quoting(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        // The unquoted character set Xcode actually relies on.
        if (c == '_' || c == '$' || c == '/' || c == ':' || c == '.' || c == '-')
            continue;
        return true;
    }
    return false;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    out += "\"";
    return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

class Parser {
public:
    Parser(const std::string& text) : s_(text) {}

    ValuePtr parse_root(std::string* error) {
        skip_ws();
        ValuePtr v = parse_value();
        if (!v) {
            if (error) *error = err_.empty() ? "empty or malformed plist" : err_;
            return nullptr;
        }
        if (!err_.empty()) {
            if (error) *error = err_;
            return nullptr;
        }
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    std::string err_;
    std::string pending_comment_;

    void fail(const std::string& msg) {
        if (err_.empty())
            err_ = "offset " + std::to_string(i_) + ": " + msg;
    }

    // Skip whitespace and comments, remembering the last /* ... */ so it can be
    // attached to the value that follows.
    void skip_ws() {
        for (;;) {
            while (i_ < s_.size() &&
                   (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
                i_++;
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') i_++;
                continue;
            }
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '*') {
                size_t end = s_.find("*/", i_ + 2);
                if (end == std::string::npos) { i_ = s_.size(); return; }
                pending_comment_ = trim(s_.substr(i_ + 2, end - i_ - 2));
                i_ = end + 2;
                continue;
            }
            return;
        }
    }

    std::string take_comment() {
        std::string c = pending_comment_;
        pending_comment_.clear();
        return c;
    }

    std::string parse_quoted() {
        std::string out;
        i_++;   // opening quote
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                char n = s_[i_ + 1];
                switch (n) {
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case 'U': {
                        // \Uxxxx escapes appear in localised strings.
                        if (i_ + 5 < s_.size()) {
                            std::string hex = s_.substr(i_ + 2, 4);
                            out += "\\U" + hex;   // preserve verbatim
                            i_ += 4;
                        }
                        break;
                    }
                    default: out += n; break;
                }
                i_ += 2;
                continue;
            }
            if (c == '"') { i_++; return out; }
            out += c;
            i_++;
        }
        fail("unterminated quoted string");
        return out;
    }

    std::string parse_bare() {
        size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
                c == '/' || c == ':' || c == '.' || c == '-' || c == '+' ||
                c == '*' || c == '~' || c == '@' || c == '<' || c == '>' ||
                c == '!' || c == '?' || c == '^') {
                // '/' may start a comment rather than continue a token.
                if (c == '/' && i_ + 1 < s_.size() &&
                    (s_[i_ + 1] == '/' || s_[i_ + 1] == '*'))
                    break;
                i_++;
                continue;
            }
            break;
        }
        return s_.substr(start, i_ - start);
    }

    ValuePtr parse_value() {
        skip_ws();
        if (i_ >= s_.size()) return nullptr;

        char c = s_[i_];
        if (c == '{') return parse_dict();
        if (c == '(') return parse_array();
        if (c == '"') {
            std::string v = parse_quoted();
            auto out = Value::make_string(v);
            skip_ws();
            out->comment = take_comment();
            return out;
        }
        std::string bare = parse_bare();
        if (bare.empty()) {
            fail(std::string("unexpected character '") + c + "'");
            i_++;
            return nullptr;
        }
        auto out = Value::make_string(bare);
        skip_ws();
        out->comment = take_comment();
        return out;
    }

    ValuePtr parse_dict() {
        auto dict = Value::make_dict();
        i_++;   // '{'
        for (;;) {
            skip_ws();
            if (i_ >= s_.size()) { fail("unterminated dictionary"); return dict; }
            if (s_[i_] == '}') { i_++; break; }

            std::string key = (s_[i_] == '"') ? parse_quoted() : parse_bare();
            if (key.empty()) {
                fail("expected a dictionary key");
                i_++;
                continue;
            }
            skip_ws();
            std::string key_comment = take_comment();

            if (i_ >= s_.size() || s_[i_] != '=') {
                fail("expected '=' after key " + key);
                return dict;
            }
            i_++;   // '='

            ValuePtr v = parse_value();
            if (!v) { fail("missing value for key " + key); return dict; }
            if (!key_comment.empty() && v->comment.empty()) v->comment = key_comment;

            dict->entries.emplace_back(key, v);

            skip_ws();
            if (i_ < s_.size() && s_[i_] == ';') { i_++; continue; }
            // A missing semicolon before '}' is tolerated.
            if (i_ < s_.size() && s_[i_] == '}') continue;
            if (i_ >= s_.size()) { fail("unterminated dictionary"); return dict; }
            fail("expected ';' after the value for " + key);
            return dict;
        }
        skip_ws();
        dict->comment = take_comment();
        return dict;
    }

    ValuePtr parse_array() {
        auto arr = Value::make_array();
        i_++;   // '('
        for (;;) {
            skip_ws();
            if (i_ >= s_.size()) { fail("unterminated array"); return arr; }
            if (s_[i_] == ')') { i_++; break; }

            ValuePtr v = parse_value();
            if (!v) { fail("malformed array element"); return arr; }
            arr->items.push_back(v);

            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
            if (i_ < s_.size() && s_[i_] == ')') continue;
            fail("expected ',' or ')' in array");
            return arr;
        }
        skip_ws();
        arr->comment = take_comment();
        return arr;
    }
};

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

void emit(const ValuePtr& v, std::string* out, int indent, bool one_line);

std::string ind(int n) { return std::string(static_cast<size_t>(n) * 2, ' '); }

void emit_key_value(const std::string& key, const ValuePtr& v, std::string* out,
                    int indent, bool one_line) {
    *out += ind(indent);
    *out += needs_quoting(key) ? quote(key) : key;
    if (!v->comment.empty() && v->is_string())
        ;   // the comment goes after the value for scalars
    *out += " = ";
    emit(v, out, indent, one_line);
    *out += ";";
    *out += one_line ? " " : "\n";
}

void emit(const ValuePtr& v, std::string* out, int indent, bool one_line) {
    switch (v->type) {
        case Value::Type::String:
            *out += needs_quoting(v->str) ? quote(v->str) : v->str;
            if (!v->comment.empty()) *out += " /* " + v->comment + " */";
            break;

        case Value::Type::Dict: {
            *out += "{";
            *out += one_line ? " " : "\n";
            for (const auto& [k, child] : v->entries)
                emit_key_value(k, child, out, one_line ? 0 : indent + 1, one_line);
            if (!one_line) *out += ind(indent);
            *out += "}";
            if (!v->comment.empty()) *out += " /* " + v->comment + " */";
            break;
        }

        case Value::Type::Array: {
            *out += "(";
            *out += one_line ? "" : "\n";
            for (const ValuePtr& item : v->items) {
                if (!one_line) *out += ind(indent + 1);
                emit(item, out, indent + 1, one_line);
                *out += ",";
                *out += one_line ? " " : "\n";
            }
            if (!one_line) *out += ind(indent);
            *out += ")";
            if (!v->comment.empty()) *out += " /* " + v->comment + " */";
            break;
        }
    }
}

// Xcode groups the objects dictionary by isa with banner comments, and writes
// the small object types on one line. Reproducing that keeps diffs readable.
bool isa_is_compact(const std::string& isa) {
    return isa == "PBXBuildFile" || isa == "PBXFileReference";
}

void emit_objects(const ValuePtr& objects, std::string* out) {
    // Bucket the object ids by isa, preserving their original order inside each
    // bucket, and emit the buckets in alphabetical isa order as Xcode does.
    std::map<std::string, std::vector<std::pair<std::string, ValuePtr>>> buckets;
    for (const auto& [id, obj] : objects->entries) {
        std::string isa = obj->is_dict() ? obj->get_string("isa") : "";
        if (isa.empty()) isa = "Unknown";
        buckets[isa].emplace_back(id, obj);
    }

    *out += "\t\tobjects = {\n";
    bool first = true;
    for (const auto& [isa, items] : buckets) {
        if (!first) *out += "\n";
        first = false;
        *out += "/* Begin " + isa + " section */\n";
        for (const auto& [id, obj] : items) {
            *out += "\t\t\t";
            *out += needs_quoting(id) ? quote(id) : id;
            if (!obj->comment.empty()) *out += " /* " + obj->comment + " */";
            *out += " = ";
            std::string body;
            emit(obj, &body, 3, isa_is_compact(isa));
            *out += body;
            *out += ";\n";
        }
        *out += "/* End " + isa + " section */\n";
    }
    *out += "\t\t};\n";
}

} // namespace

ValuePtr parse(const std::string& text, std::string* error) {
    Parser p(text);
    return p.parse_root(error);
}

std::string serialize(const ValuePtr& root, bool pbxproj_style) {
    if (!root) return "";

    if (!pbxproj_style) {
        std::string out;
        emit(root, &out, 0, false);
        out += "\n";
        return out;
    }

    // Xcode's own header; without it the project may be treated as a different
    // encoding.
    std::string out = "// !$*UTF8*$!\n{\n";
    for (const auto& [k, v] : root->entries) {
        if (k == "objects" && v->is_dict()) {
            emit_objects(v, &out);
            continue;
        }
        out += "\t\t";
        out += needs_quoting(k) ? quote(k) : k;
        out += " = ";
        std::string body;
        emit(v, &body, 2, false);
        out += body;
        out += ";\n";
    }
    out += "}\n";
    return out;
}

} // namespace ppcode::plist

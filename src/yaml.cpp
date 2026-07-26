#include "yaml.hpp"

#include <cctype>
#include <cstdlib>

namespace ppcode::yaml {

namespace {

struct Line {
    int indent = 0;
    std::string text;      // content with indentation stripped, comments removed
    std::string raw;       // original, needed verbatim for block scalars
    int number = 0;        // 1-based, for error messages
    bool blank = false;
};

bool is_blank_or_comment(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        return c == '#';
    }
    return true;
}

// Strip a trailing comment, respecting quotes. A '#' only starts a comment when
// preceded by whitespace or at the start of the value.
std::string strip_comment(const std::string& s) {
    bool in_single = false, in_double = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        else if (c == '#' && !in_single && !in_double) {
            if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')
                return s.substr(0, i);
        }
    }
    return s;
}

int measure_indent(const std::string& s) {
    int n = 0;
    for (char c : s) {
        if (c == ' ') n++;
        else break;
    }
    return n;
}

std::string unquote(const std::string& s, bool* was_quoted) {
    *was_quoted = false;
    if (s.size() >= 2) {
        if (s.front() == '\'' && s.back() == '\'') {
            *was_quoted = true;
            // In single quotes, '' is a literal apostrophe.
            std::string inner = s.substr(1, s.size() - 2);
            std::string out;
            for (size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') {
                    out += '\'';
                    i++;
                } else {
                    out += inner[i];
                }
            }
            return out;
        }
        if (s.front() == '"' && s.back() == '"') {
            *was_quoted = true;
            std::string inner = s.substr(1, s.size() - 2);
            std::string out;
            for (size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '\\' && i + 1 < inner.size()) {
                    char n = inner[++i];
                    switch (n) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case '0': out += '\0'; break;
                        case '\\': out += '\\'; break;
                        case '"': out += '"'; break;
                        default: out += n; break;
                    }
                } else {
                    out += inner[i];
                }
            }
            return out;
        }
    }
    return s;
}

// Find the top-level ':' separating a key from its value, ignoring anything
// inside quotes or flow collections.
size_t find_key_colon(const std::string& s) {
    bool in_single = false, in_double = false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_single;
        else if (in_single || in_double) continue;
        else if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') depth--;
        else if (c == ':' && depth == 0) {
            // A key colon is followed by whitespace or ends the line.
            if (i + 1 >= s.size() || s[i + 1] == ' ' || s[i + 1] == '\t') return i;
        }
    }
    return std::string::npos;
}

// Split "a, b, {c: 1}" at top-level commas.
std::vector<std::string> split_flow(const std::string& s) {
    std::vector<std::string> out;
    bool in_single = false, in_double = false;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '\'' && !in_double) { in_single = !in_single; cur += c; continue; }
        if (c == '"' && !in_single)  { in_double = !in_double; cur += c; continue; }
        if (!in_single && !in_double) {
            if (c == '[' || c == '{') depth++;
            if (c == ']' || c == '}') depth--;
            if (c == ',' && depth == 0) {
                out.push_back(trim(cur));
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

class Parser {
public:
    Parser(std::vector<Line> lines) : lines_(std::move(lines)) {}

    bool run(json* out, std::string* error) {
        size_t i = 0;
        skip_blanks(&i);
        if (i >= lines_.size()) {
            *out = json::object();
            return true;
        }
        int indent = lines_[i].indent;
        json v = parse_block(&i, indent);
        if (!error_.empty()) {
            if (error) *error = error_;
            return false;
        }
        *out = v;
        return true;
    }

private:
    std::vector<Line> lines_;
    std::string error_;

    void fail(int line, const std::string& msg) {
        if (error_.empty())
            error_ = "line " + std::to_string(line) + ": " + msg;
    }

    void skip_blanks(size_t* i) {
        while (*i < lines_.size() && lines_[*i].blank) (*i)++;
    }

    json parse_flow(const std::string& s, int line) {
        std::string t = trim(s);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            json arr = json::array();
            for (const std::string& item : split_flow(t.substr(1, t.size() - 2)))
                arr.push_back(parse_flow(item, line));
            return arr;
        }
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') {
            json obj = json::object();
            for (const std::string& item : split_flow(t.substr(1, t.size() - 2))) {
                size_t c = find_key_colon(item);
                if (c == std::string::npos) {
                    fail(line, "expected key: value inside { }");
                    continue;
                }
                bool q = false;
                std::string k = unquote(trim(item.substr(0, c)), &q);
                obj[k] = parse_flow(trim(item.substr(c + 1)), line);
            }
            return obj;
        }
        bool quoted = false;
        std::string v = unquote(t, &quoted);
        return quoted ? json(v) : infer_scalar(v);
    }

    // A block scalar: "|", "|-", ">", ">-" plus the indented lines beneath.
    json parse_block_scalar(size_t* i, const std::string& header, int parent_indent) {
        bool folded = !header.empty() && header[0] == '>';
        bool strip = header.find('-') != std::string::npos;
        bool keep = header.find('+') != std::string::npos;
        (*i)++;

        // The block's indentation is that of its first non-blank line.
        int block_indent = -1;
        std::vector<std::string> content;
        while (*i < lines_.size()) {
            const Line& l = lines_[*i];
            if (l.blank) {
                content.push_back("");
                (*i)++;
                continue;
            }
            int ind = measure_indent(l.raw);
            if (ind <= parent_indent) break;
            if (block_indent < 0) block_indent = ind;
            if (ind < block_indent) break;
            content.push_back(l.raw.substr(static_cast<size_t>(block_indent)));
            (*i)++;
        }
        // Trailing blank lines are not part of the value unless kept.
        while (!content.empty() && content.back().empty() && !keep) content.pop_back();

        std::string out;
        if (folded) {
            // Fold single newlines into spaces; blank lines become newlines.
            for (size_t k = 0; k < content.size(); k++) {
                if (content[k].empty()) {
                    out += "\n";
                } else {
                    if (!out.empty() && out.back() != '\n') out += " ";
                    out += content[k];
                }
            }
        } else {
            for (const std::string& l : content) {
                out += l;
                out += "\n";
            }
        }
        if (strip) {
            while (!out.empty() && out.back() == '\n') out.pop_back();
        } else if (!folded && !out.empty() && out.back() != '\n') {
            out += "\n";
        }
        return json(out);
    }

    json parse_block(size_t* i, int indent) {
        skip_blanks(i);
        if (*i >= lines_.size()) return json();

        if (starts_with(lines_[*i].text, "- ") || lines_[*i].text == "-")
            return parse_sequence(i, indent);
        return parse_mapping(i, indent);
    }

    json parse_sequence(size_t* i, int indent) {
        json arr = json::array();
        while (true) {
            skip_blanks(i);
            if (*i >= lines_.size()) break;
            const Line& l = lines_[*i];
            if (l.indent != indent) break;
            if (!starts_with(l.text, "- ") && l.text != "-") break;

            int line_no = l.number;
            std::string rest = (l.text == "-") ? "" : trim(l.text.substr(2));

            if (rest.empty()) {
                // Value lives on the following, more-indented lines.
                (*i)++;
                skip_blanks(i);
                if (*i < lines_.size() && lines_[*i].indent > indent)
                    arr.push_back(parse_block(i, lines_[*i].indent));
                else
                    arr.push_back(json());
                continue;
            }

            // "- key: value" starts a mapping whose remaining keys are indented
            // to line up under the text after the dash.
            size_t colon = find_key_colon(rest);
            if (colon != std::string::npos && rest.front() != '{' && rest.front() != '[') {
                int virt_indent = indent + 2;
                // Rewrite this line as a plain mapping line at virt_indent, then
                // let parse_mapping consume it along with its siblings.
                Line rewritten = l;
                rewritten.indent = virt_indent;
                rewritten.text = rest;
                rewritten.raw = std::string(static_cast<size_t>(virt_indent), ' ') + rest;
                lines_[*i] = rewritten;
                arr.push_back(parse_mapping(i, virt_indent));
                continue;
            }

            arr.push_back(parse_flow(rest, line_no));
            (*i)++;
        }
        return arr;
    }

    json parse_mapping(size_t* i, int indent) {
        json obj = json::object();
        while (true) {
            skip_blanks(i);
            if (*i >= lines_.size()) break;
            const Line& l = lines_[*i];
            if (l.indent != indent) {
                if (l.indent > indent) {
                    fail(l.number, "unexpected indentation");
                    (*i)++;
                    continue;
                }
                break;
            }
            if (starts_with(l.text, "- ")) break;   // a sequence at our level

            size_t colon = find_key_colon(l.text);
            if (colon == std::string::npos) {
                fail(l.number, "expected \"key: value\", got: " + elide(l.text, 60));
                (*i)++;
                continue;
            }

            bool kq = false;
            std::string key = unquote(trim(l.text.substr(0, colon)), &kq);
            std::string rest = trim(l.text.substr(colon + 1));
            int line_no = l.number;

            if (key.empty()) {
                fail(line_no, "empty key");
                (*i)++;
                continue;
            }

            if (!rest.empty() && (rest[0] == '|' || rest[0] == '>')) {
                obj[key] = parse_block_scalar(i, rest, indent);
                continue;
            }

            if (rest.empty()) {
                (*i)++;
                skip_blanks(i);
                if (*i < lines_.size() && lines_[*i].indent > indent) {
                    obj[key] = parse_block(i, lines_[*i].indent);
                } else if (*i < lines_.size() && lines_[*i].indent == indent &&
                           starts_with(lines_[*i].text, "- ")) {
                    // A sequence indented level with its key, which is legal.
                    obj[key] = parse_sequence(i, indent);
                } else {
                    obj[key] = json();
                }
                continue;
            }

            obj[key] = parse_flow(rest, line_no);
            (*i)++;
        }
        return obj;
    }
};

} // namespace

json infer_scalar(const std::string& s) {
    if (s.empty()) return json();
    std::string low = to_lower(s);
    if (low == "null" || low == "~") return json();
    if (low == "true" || low == "yes" || low == "on") return json(true);
    if (low == "false" || low == "no" || low == "off") return json(false);

    // Integer?
    {
        bool ok = true;
        size_t start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
        if (start >= s.size()) ok = false;
        for (size_t i = start; i < s.size() && ok; i++)
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) ok = false;
        if (ok) {
            errno = 0;
            long long v = std::strtoll(s.c_str(), nullptr, 10);
            if (errno == 0) return json(v);
        }
    }
    // Float? Require a digit somewhere and only float-ish characters.
    {
        bool ok = true, seen_digit = false;
        for (size_t i = 0; i < s.size() && ok; i++) {
            char c = s[i];
            if (std::isdigit(static_cast<unsigned char>(c))) seen_digit = true;
            else if (c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') continue;
            else ok = false;
        }
        if (ok && seen_digit) {
            char* end = nullptr;
            double v = std::strtod(s.c_str(), &end);
            if (end && *end == '\0') return json(v);
        }
    }
    return json(s);
}

bool parse(const std::string& text, json* out, std::string* error) {
    std::vector<Line> lines;
    int n = 0;
    for (const std::string& raw_line : split(text, '\n')) {
        std::string raw = raw_line;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        n++;

        Line l;
        l.raw = raw;
        l.number = n;
        if (is_blank_or_comment(raw)) {
            l.blank = true;
            l.indent = 0;
            lines.push_back(l);
            continue;
        }
        // Tabs for indentation are illegal in YAML and a common silent error.
        for (char c : raw) {
            if (c == '\t') {
                if (error)
                    *error = "line " + std::to_string(n) +
                             ": tab used for indentation; YAML requires spaces";
                return false;
            }
            if (c != ' ') break;
        }
        l.indent = measure_indent(raw);
        l.text = trim(strip_comment(raw));
        lines.push_back(l);
    }

    Parser p(std::move(lines));
    return p.run(out, error);
}

bool split_frontmatter(const std::string& text, std::string* front,
                       std::string* body, std::string* error) {
    front->clear();
    body->clear();

    std::vector<std::string> lines = split(text, '\n');
    size_t i = 0;
    // Tolerate a leading blank line or a BOM before the fence.
    while (i < lines.size() && trim(lines[i]).empty()) i++;

    if (i >= lines.size() || trim(lines[i]) != "---") {
        *body = text;
        return true;
    }

    size_t start = i + 1;
    size_t end = std::string::npos;
    for (size_t k = start; k < lines.size(); k++) {
        std::string t = trim(lines[k]);
        if (t == "---" || t == "...") { end = k; break; }
    }
    if (end == std::string::npos) {
        if (error)
            *error = "frontmatter opened with --- but never closed "
                     "(expected a closing --- line)";
        return false;
    }

    std::vector<std::string> fm(lines.begin() + static_cast<long>(start),
                               lines.begin() + static_cast<long>(end));
    std::vector<std::string> rest(lines.begin() + static_cast<long>(end) + 1,
                                  lines.end());
    *front = join(fm, "\n");
    *body = join(rest, "\n");
    return true;
}

} // namespace ppcode::yaml

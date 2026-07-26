#include "mdparse.hpp"

#include "utf8.hpp"

#include <cctype>
#include <cstring>

namespace ppcode::md {

namespace {

bool is_blank(const std::string& s) {
    return s.find_first_not_of(" \t") == std::string::npos;
}

size_t indent_of(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return i;
}

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// ---------------------------------------------------------------------------
// Line classification
// ---------------------------------------------------------------------------

// An opening fence: three or more of ` or ~ at an indent of at most three.
bool fence_open(const std::string& line, char* ch, size_t* len, std::string* info) {
    size_t lead = indent_of(line);
    if (lead > 3 || lead >= line.size()) return false;

    char c = line[lead];
    if (c != '`' && c != '~') return false;

    size_t n = 0;
    while (lead + n < line.size() && line[lead + n] == c) n++;
    if (n < 3) return false;

    std::string rest = trim(line.substr(lead + n));
    // A backtick fence cannot carry a backtick in its info string, which is
    // what stops ``` inline code from being mistaken for a fence.
    if (c == '`' && rest.find('`') != std::string::npos) return false;

    *ch = c;
    *len = n;
    *info = rest;
    return true;
}

bool fence_close(const std::string& line, char ch, size_t len) {
    size_t lead = indent_of(line);
    if (lead > 3) return false;

    size_t n = 0;
    while (lead + n < line.size() && line[lead + n] == ch) n++;
    if (n < len) return false;

    return trim(line.substr(lead + n)).empty();
}

bool thematic_break(const std::string& line) {
    std::string t = trim(line);
    if (t.size() < 3) return false;

    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;

    size_t count = 0;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == c) count++;
        else if (t[i] != ' ') return false;
    }

    return count >= 3;
}

bool atx_heading(const std::string& line, int* level, std::string* body) {
    size_t lead = indent_of(line);
    if (lead > 3) return false;

    size_t i = lead, n = 0;
    while (i < line.size() && line[i] == '#') { i++; n++; }
    if (n < 1 || n > 6) return false;
    // "#tag" is not a heading; the marker has to be followed by a space.
    if (i < line.size() && line[i] != ' ') return false;

    std::string b = trim(line.substr(i));

    // A closing run of hashes is decoration, not content.
    size_t end = b.size();
    while (end > 0 && b[end - 1] == '#') end--;
    if (end < b.size() && (end == 0 || b[end - 1] == ' ')) b = trim(b.substr(0, end));

    *level = static_cast<int>(n);
    *body = b;
    return true;
}

struct ListMark {
    size_t lead = 0;
    bool ordered = false;
    std::string marker;
    size_t body_off = 0;
};

bool list_item(const std::string& line, ListMark* m) {
    size_t lead = indent_of(line);
    if (lead >= line.size()) return false;

    char c = line[lead];
    if ((c == '-' || c == '*' || c == '+') && lead + 1 < line.size() &&
        line[lead + 1] == ' ') {
        m->lead = lead;
        m->ordered = false;
        m->marker = std::string(1, c);
        m->body_off = lead + 2;
        return true;
    }

    size_t j = lead;
    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) j++;
    if (j > lead && j < line.size() && (line[j] == '.' || line[j] == ')') &&
        j + 1 < line.size() && line[j + 1] == ' ') {
        m->lead = lead;
        m->ordered = true;
        m->marker = line.substr(lead, j - lead + 1);
        m->body_off = j + 2;
        return true;
    }

    return false;
}

// The row of dashes under a table header. Pipes are required by the caller, so
// this only has to reject rows carrying real content.
bool table_delimiter(const std::string& line) {
    std::string t = trim(line);
    if (t.empty()) return false;

    bool saw_dash = false;
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (c == '-') saw_dash = true;
        else if (c != '|' && c != ':' && c != ' ') return false;
    }

    return saw_dash;
}

std::vector<std::string> table_cells(const std::string& line) {
    std::string t = trim(line);
    if (!t.empty() && t[0] == '|') t.erase(t.begin());
    if (!t.empty() && t[t.size() - 1] == '|') t.erase(t.size() - 1);

    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cur += '|';
            i++;
            continue;
        }

        if (t[i] == '|') {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }

        cur += t[i];
    }
    out.push_back(trim(cur));

    return out;
}

// ---------------------------------------------------------------------------
// Inline markup
// ---------------------------------------------------------------------------

void push_run(std::vector<Run>* out, const std::string& text, unsigned style,
              const std::string& href) {
    if (text.empty()) return;

    if (!out->empty() && out->back().style == style && out->back().href == href) {
        out->back().text += text;
        return;
    }

    Run r;
    r.text = text;
    r.style = style;
    r.href = href;
    out->push_back(r);
}

// Emphasis nests, so this recurses with the enclosing style as the base. That
// is the whole reason for not reusing render.cpp's inline_spans, which can only
// carry one Style per span and so cannot express bold-and-italic at once.
void inline_into(const std::string& s, unsigned base, const std::string& href,
                 std::vector<Run>* out) {
    std::string buf;
    size_t i = 0;
    const size_t n = s.size();

    while (i < n) {
        char c = s[i];

        // A backslash makes the next punctuation character literal.
        if (c == '\\' && i + 1 < n &&
            std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            buf += s[i + 1];
            i += 2;
            continue;
        }

        // `code`, or ``code containing a ` `` -- the closing run has to be
        // exactly as long as the opening one.
        if (c == '`') {
            size_t open = 0;
            while (i + open < n && s[i + open] == '`') open++;

            size_t j = i + open;
            size_t end = std::string::npos;
            while (j < n) {
                if (s[j] != '`') { j++; continue; }

                size_t k = j;
                while (k < n && s[k] == '`') k++;
                if (k - j == open) { end = j; break; }
                j = k;
            }

            if (end != std::string::npos) {
                push_run(out, buf, base, href);
                buf.clear();
                std::string code = s.substr(i + open, end - i - open);
                // One padding space on each side is a delimiter, not content.
                if (code.size() >= 2 && code[0] == ' ' && code[code.size() - 1] == ' ')
                    code = code.substr(1, code.size() - 2);
                push_run(out, code, base | StyleCode, href);
                i = end + open;
                continue;
            }
        }

        // ~~strikethrough~~
        if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            size_t end = s.find("~~", i + 2);
            if (end != std::string::npos && end > i + 2) {
                push_run(out, buf, base, href);
                buf.clear();
                inline_into(s.substr(i + 2, end - i - 2), base | StyleStrike, href, out);
                i = end + 2;
                continue;
            }
        }

        // ***both*** -- bold and italic at once. Checked before the two-marker
        // forms, which would otherwise match the first two stars and leave the
        // third as literal text.
        if ((c == '*' || c == '_') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
            std::string delim(3, c);
            size_t end = s.find(delim, i + 3);
            if (end != std::string::npos && end > i + 3) {
                push_run(out, buf, base, href);
                buf.clear();
                inline_into(s.substr(i + 3, end - i - 3),
                            base | StyleBold | StyleItalic, href, out);
                i = end + 3;
                continue;
            }
        }

        // **bold** or __bold__
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
            std::string delim(2, c);
            size_t end = s.find(delim, i + 2);
            bool ok = (end != std::string::npos) && end > i + 2;
            // Underscores never open or close inside a word. Without this,
            // MAX__VALUE and similar identifiers come out italic, which in a
            // coding tool is worse than missing the odd emphasis.
            if (ok && c == '_') {
                if (i > 0 && word_char(s[i - 1])) ok = false;
                if (end + 2 < n && word_char(s[end + 2])) ok = false;
            }

            if (ok) {
                push_run(out, buf, base, href);
                buf.clear();
                inline_into(s.substr(i + 2, end - i - 2), base | StyleBold, href, out);
                i = end + 2;
                continue;
            }
        }

        // *italic* or _italic_. A space after the opener rules it out, so
        // "2 * 3 * 4" stays arithmetic.
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] != ' ') {
            size_t end = s.find(c, i + 1);
            bool ok = (end != std::string::npos) && end > i + 1 && s[end - 1] != ' ';
            if (ok && c == '_') {
                if (i > 0 && word_char(s[i - 1])) ok = false;
                if (end + 1 < n && word_char(s[end + 1])) ok = false;
            }

            if (ok) {
                push_run(out, buf, base, href);
                buf.clear();
                inline_into(s.substr(i + 1, end - i - 1), base | StyleItalic, href, out);
                i = end + 1;
                continue;
            }
        }

        // [label](url)
        if (c == '[') {
            size_t close = s.find(']', i + 1);
            if (close != std::string::npos && close + 1 < n && s[close + 1] == '(') {
                size_t paren = s.find(')', close + 2);
                if (paren != std::string::npos) {
                    std::string label = s.substr(i + 1, close - i - 1);
                    std::string url = trim(s.substr(close + 2, paren - close - 2));
                    // A quoted title after the target is not part of it.
                    size_t sp = url.find(' ');
                    if (sp != std::string::npos) url = url.substr(0, sp);
                    if (label.empty()) label = url;

                    push_run(out, buf, base, href);
                    buf.clear();
                    inline_into(label, base | StyleLink, url, out);
                    i = paren + 1;
                    continue;
                }
            }
        }

        // <http://example.com> autolink
        if (c == '<') {
            size_t close = s.find('>', i + 1);
            if (close != std::string::npos) {
                std::string url = s.substr(i + 1, close - i - 1);
                if (starts_with(url, "http://") || starts_with(url, "https://") ||
                    starts_with(url, "mailto:")) {
                    push_run(out, buf, base, href);
                    buf.clear();
                    push_run(out, url, base | StyleLink, url);
                    i = close + 1;
                    continue;
                }
            }
        }

        // A bare URL, which models emit far more often than a proper link.
        if (c == 'h' && (i == 0 || !word_char(s[i - 1])) &&
            (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0)) {
            size_t j = i;
            while (j < n && s[j] != ' ' && s[j] != '<' && s[j] != '>') j++;
            // Sentence punctuation at the end belongs to the sentence.
            while (j > i && std::strchr(".,;:!?)]}", s[j - 1]) != NULL) j--;

            push_run(out, buf, base, href);
            buf.clear();
            std::string url = s.substr(i, j - i);
            push_run(out, url, base | StyleLink, url);
            i = j;
            continue;
        }

        buf += c;
        i++;
    }

    push_run(out, buf, base, href);
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

std::vector<Node> parse_lines(const std::vector<std::string>& lines, int quote);

// Everything a paragraph has to stop at.
bool starts_new_block(const std::string& l) {
    int lv = 0;
    std::string b;
    ListMark m;
    char fc = 0;
    size_t fn = 0;
    std::string info;

    if (thematic_break(l)) return true;
    if (atx_heading(l, &lv, &b)) return true;
    if (list_item(l, &m)) return true;
    if (fence_open(l, &fc, &fn, &info)) return true;

    size_t lead = indent_of(l);
    if (lead <= 3 && lead < l.size() && l[lead] == '>') return true;

    return false;
}

std::vector<Node> parse_lines(const std::vector<std::string>& lines, int quote) {
    std::vector<Node> out;
    // Open list nesting, as the indent column of each level. A list item's
    // depth is where its indent lands in this stack, which handles the mix of
    // two- and four-space nesting that models produce.
    std::vector<size_t> list_indents;
    size_t i = 0;

    while (i < lines.size()) {
        const std::string& line = lines[i];

        if (is_blank(line)) {
            i++;
            continue;
        }

        // Fenced code
        char fc = 0;
        size_t fn = 0;
        std::string info;
        if (fence_open(line, &fc, &fn, &info)) {
            std::vector<std::string> body;
            i++;

            while (i < lines.size() && !fence_close(lines[i], fc, fn)) {
                body.push_back(lines[i]);
                i++;
            }

            if (i < lines.size()) i++;   // the closing fence itself

            Node n;
            n.kind = Block::Code;
            n.quote = quote;
            n.lang = info;
            n.text = join(body, "\n");
            out.push_back(n);
            list_indents.clear();
            continue;
        }

        // A thematic break, checked before the list rule so "---" is a rule
        // rather than an empty bullet.
        if (thematic_break(line)) {
            Node n;
            n.kind = Block::Rule;
            n.quote = quote;
            out.push_back(n);
            i++;
            list_indents.clear();
            continue;
        }

        int level = 0;
        std::string body;
        if (atx_heading(line, &level, &body)) {
            Node n;
            n.kind = Block::Heading;
            n.level = level;
            n.quote = quote;
            n.runs = parse_inline(body);
            out.push_back(n);
            i++;
            list_indents.clear();
            continue;
        }

        // Blockquote. The marker is stripped and the remainder parsed again, so
        // a list or a code block inside a quote arrives as the node it really
        // is with quote incremented, rather than as quoted text.
        size_t lead = indent_of(line);
        if (lead <= 3 && lead < line.size() && line[lead] == '>') {
            std::vector<std::string> inner;

            while (i < lines.size()) {
                size_t l2 = indent_of(lines[i]);
                if (l2 > 3 || l2 >= lines[i].size() || lines[i][l2] != '>') break;

                std::string rest = lines[i].substr(l2 + 1);
                if (!rest.empty() && rest[0] == ' ') rest.erase(rest.begin());
                inner.push_back(rest);
                i++;
            }

            std::vector<Node> sub = parse_lines(inner, quote + 1);
            for (size_t k = 0; k < sub.size(); k++) out.push_back(sub[k]);
            list_indents.clear();
            continue;
        }

        // Pipe table. The delimiter row is what distinguishes a table from a
        // paragraph that happens to contain a pipe.
        if (line.find('|') != std::string::npos && i + 1 < lines.size() &&
            lines[i + 1].find('|') != std::string::npos &&
            table_delimiter(lines[i + 1])) {
            std::vector<std::string> rows;
            rows.push_back(line);
            i += 2;                       // header plus the delimiter row

            while (i < lines.size() && !is_blank(lines[i]) &&
                   lines[i].find('|') != std::string::npos) {
                rows.push_back(lines[i]);
                i++;
            }

            for (size_t r = 0; r < rows.size(); r++) {
                Node n;
                n.kind = Block::TableRow;
                n.quote = quote;
                n.header = (r == 0);
                n.table_start = (r == 0);
                n.table_end = (r + 1 == rows.size());

                std::vector<std::string> cs = table_cells(rows[r]);
                for (size_t c = 0; c < cs.size(); c++)
                    n.cells.push_back(parse_inline(cs[c]));

                out.push_back(n);
            }

            list_indents.clear();
            continue;
        }

        // List item
        ListMark m;
        if (list_item(line, &m)) {
            while (!list_indents.empty() && list_indents.back() > m.lead)
                list_indents.pop_back();
            if (list_indents.empty() || list_indents.back() < m.lead)
                list_indents.push_back(m.lead);

            std::string text = line.substr(m.body_off);
            i++;

            // Continuation lines indented past the marker belong to this item.
            while (i < lines.size() && !is_blank(lines[i])) {
                ListMark m2;
                if (list_item(lines[i], &m2)) break;
                if (indent_of(lines[i]) <= m.lead) break;
                if (starts_new_block(lines[i])) break;

                text += " ";
                text += trim(lines[i]);
                i++;
            }

            Node n;
            n.kind = m.ordered ? Block::Numbered : Block::Bullet;
            n.level = static_cast<int>(list_indents.size()) - 1;
            n.quote = quote;
            n.marker = m.marker;
            n.runs = parse_inline(trim(text));
            out.push_back(n);
            continue;
        }

        // Indented code, but only outside a list: four spaces under a bullet is
        // that bullet's continuation, not a code block.
        if (list_indents.empty() && starts_with(line, "    ")) {
            std::vector<std::string> body;

            while (i < lines.size()) {
                if (is_blank(lines[i])) {
                    // Blank lines belong to the block only if indented code
                    // resumes after them.
                    size_t j = i;
                    while (j < lines.size() && is_blank(lines[j])) j++;
                    if (j >= lines.size() || !starts_with(lines[j], "    ")) break;

                    while (i < j) {
                        body.push_back("");
                        i++;
                    }
                    continue;
                }

                if (!starts_with(lines[i], "    ")) break;

                body.push_back(lines[i].substr(4));
                i++;
            }

            Node n;
            n.kind = Block::Code;
            n.quote = quote;
            n.text = join(body, "\n");
            out.push_back(n);
            continue;
        }

        // Paragraph, running until something else begins.
        {
            std::vector<std::string> para;
            int setext = 0;

            while (i < lines.size()) {
                if (is_blank(lines[i])) break;

                if (!para.empty()) {
                    // A run of = or - under a paragraph is a setext heading.
                    std::string t = trim(lines[i]);
                    if (!t.empty() &&
                        (t.find_first_not_of('=') == std::string::npos ||
                         t.find_first_not_of('-') == std::string::npos)) {
                        setext = (t[0] == '=') ? 1 : 2;
                        i++;
                        break;
                    }

                    if (starts_new_block(lines[i])) break;
                }

                para.push_back(lines[i]);
                i++;
            }

            // Inside a paragraph a single newline is a space; two trailing
            // spaces make it a hard break.
            std::string text;
            bool prev_hard = false;
            for (size_t k = 0; k < para.size(); k++) {
                bool hard = ends_with(para[k], "  ");
                if (k > 0) text += prev_hard ? "\n" : " ";
                text += trim(para[k]);
                prev_hard = hard;
            }

            Node n;
            n.kind = setext ? Block::Heading : Block::Paragraph;
            n.level = setext;
            n.quote = quote;
            n.runs = parse_inline(text);
            out.push_back(n);
            list_indents.clear();
            continue;
        }
    }

    return out;
}

} // namespace

std::vector<Run> parse_inline(const std::string& text) {
    std::vector<Run> out;
    inline_into(text, 0, "", &out);
    return out;
}

std::vector<Node> parse(const std::string& text) {
    // sanitize() both repairs invalid UTF-8 and expands tabs, so the
    // indentation arithmetic below can count bytes.
    std::string t = utf8::sanitize(text);

    std::string clean;
    clean.reserve(t.size());
    for (size_t i = 0; i < t.size(); i++)
        if (t[i] != '\r') clean += t[i];

    return parse_lines(split(clean, '\n'), 0);
}

size_t complete_prefix(const std::string& text) {
    size_t committed = 0;
    size_t pos = 0;
    bool in_fence = false;
    char fence_ch = 0;
    size_t fence_len = 0;

    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) break;    // a partial trailing line

        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        pos = nl + 1;

        if (in_fence) {
            if (fence_close(line, fence_ch, fence_len)) {
                in_fence = false;
                committed = pos;               // a closed fence ends a block
            }
            continue;
        }

        char c = 0;
        size_t n = 0;
        std::string info;
        if (fence_open(line, &c, &n, &info)) {
            in_fence = true;
            fence_ch = c;
            fence_len = n;
            continue;
        }

        if (is_blank(line)) committed = pos;
    }

    return committed;
}

} // namespace ppcode::md

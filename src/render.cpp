#include "render.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace ppcode::render {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

namespace {

// A muted, reasonably colour-blind-tolerant theme. Values are chosen to survive
// reduction to 256 and to 16 colours without two adjacent roles collapsing onto
// the same slot.
struct Entry { Style s; StyleDef d; };

const Entry kTheme[] = {
    {Style::Plain,      {{0xD0, 0xD0, 0xD0}, false, false, false, false, -1}},
    {Style::Dim,        {{0x80, 0x80, 0x80}, false, false, true,  false,  7}},
    {Style::Heading1,   {{0x7F, 0xD1, 0xFF}, true,  false, false, false,  6}},
    {Style::Heading2,   {{0x6F, 0xC1, 0xEF}, true,  false, false, false,  6}},
    {Style::Heading3,   {{0x5F, 0xB1, 0xDF}, true,  false, false, false,  6}},
    {Style::Bold,       {{0xFF, 0xFF, 0xFF}, true,  false, false, false, -1}},
    {Style::Italic,     {{0xD8, 0xD8, 0xD8}, false, true,  false, false, -1}},
    {Style::InlineCode, {{0xF0, 0xC0, 0x74}, false, false, false, false,  3}},
    {Style::CodeBg,     {{0xC8, 0xC8, 0xC8}, false, false, false, false, -1}},
    {Style::Quote,      {{0x9A, 0x9A, 0x9A}, false, false, true,  false,  7}},
    {Style::ListMarker, {{0x7F, 0xD1, 0xFF}, true,  false, false, false,  6}},
    {Style::Link,       {{0x88, 0xAA, 0xFF}, false, true,  false, false,  4}},
    {Style::Rule,       {{0x60, 0x60, 0x60}, false, false, true,  false,  7}},

    {Style::Keyword,    {{0xE0, 0x8C, 0xE0}, true,  false, false, false,  5}},
    {Style::Type,       {{0x6F, 0xD0, 0xC0}, false, false, false, false,  6}},
    {Style::Constant,   {{0xFF, 0xA0, 0x70}, false, false, false, false,  3}},
    {Style::String,     {{0x9A, 0xD8, 0x7A}, false, false, false, false,  2}},
    {Style::Number,     {{0xFF, 0xB0, 0x60}, false, false, false, false,  3}},
    {Style::Comment,    {{0x78, 0x8A, 0x78}, false, false, true,  false,  7}},
    {Style::Preproc,    {{0xC0, 0x90, 0xE0}, false, false, false, false,  5}},
    {Style::Function,   {{0x8C, 0xC8, 0xFF}, false, false, false, false,  4}},
    {Style::Operator,   {{0xC0, 0xC0, 0xC0}, false, false, false, false, -1}},

    {Style::UserText,   {{0x70, 0xD0, 0xD0}, true,  false, false, false,  6}},
    {Style::ToolName,   {{0xF0, 0xC0, 0x50}, true,  false, false, false,  3}},
    {Style::ToolOutput, {{0x90, 0x90, 0x90}, false, false, true,  false,  7}},
    {Style::ErrorText,  {{0xFF, 0x6A, 0x6A}, true,  false, false, false,  1}},
    {Style::StatusText, {{0x8A, 0xD8, 0x8A}, false, false, false, false,  2}},
    {Style::Bar,        {{0x10, 0x10, 0x10}, false, false, false, true,  -1}},
    {Style::Prompt,     {{0xD8, 0x8C, 0xE8}, true,  false, false, false,  5}},
};

StyleDef g_defs[static_cast<size_t>(Style::Count_)];
bool g_defs_ready = false;

void ensure_defs() {
    if (g_defs_ready) return;
    for (const Entry& e : kTheme) g_defs[static_cast<size_t>(e.s)] = e.d;
    g_defs_ready = true;
}

} // namespace

const StyleDef& style_def(Style s) {
    ensure_defs();
    size_t i = static_cast<size_t>(s);
    if (i >= static_cast<size_t>(Style::Count_)) i = 0;
    return g_defs[i];
}

ColorDepth detect_depth(int ncurses_colors, bool has_color) {
    if (!has_color || ncurses_colors < 8) return ColorDepth::Mono;

    // ncurses reports a huge COLORS for direct-colour terminfo entries
    // (TERM=*-direct), which is the only case where we can set true RGB.
    if (ncurses_colors > 0x7FFF) return ColorDepth::TrueColor;

    const char* ct = std::getenv("COLORTERM");
    if (ct && (std::string(ct) == "truecolor" || std::string(ct) == "24bit")) {
        // The terminal claims RGB but terminfo does not expose it, so ncurses
        // cannot address it. 256 is the best we can drive through curses.
        return ncurses_colors >= 256 ? ColorDepth::Ansi256 : ColorDepth::Ansi16;
    }
    if (ncurses_colors >= 256) return ColorDepth::Ansi256;
    return ColorDepth::Ansi16;
}

std::string depth_name(ColorDepth d) {
    switch (d) {
        case ColorDepth::Mono:      return "monochrome";
        case ColorDepth::Ansi16:    return "16 colours";
        case ColorDepth::Ansi256:   return "256 colours";
        case ColorDepth::TrueColor: return "24-bit colour";
    }
    return "?";
}

int rgb_to_256(Rgb c) {
    // Greys map to the 24-step ramp when all channels are close together;
    // otherwise use the 6x6x6 cube.
    int mx = std::max({c.r, c.g, c.b});
    int mn = std::min({c.r, c.g, c.b});
    if (mx - mn < 12) {
        int level = (mx * 25) / 255;          // 0..25
        if (level <= 0) return 16;            // cube black
        if (level >= 25) return 231;          // cube white
        return 232 + (level - 1);             // 232..255
    }
    auto q = [](uint8_t v) { return (v * 5 + 127) / 255; };   // 0..5
    return 16 + 36 * q(c.r) + 6 * q(c.g) + q(c.b);
}

int rgb_to_16(Rgb c) {
    // Nearest of the 8 basic colours in RGB space; brightness is conveyed by
    // the bold attribute instead of the bright colour range.
    static const Rgb base[8] = {
        {0x00, 0x00, 0x00}, {0xAA, 0x00, 0x00}, {0x00, 0xAA, 0x00},
        {0xAA, 0xAA, 0x00}, {0x00, 0x00, 0xAA}, {0xAA, 0x00, 0xAA},
        {0x00, 0xAA, 0xAA}, {0xAA, 0xAA, 0xAA},
    };
    int best = 7;
    long bestd = -1;
    for (int i = 0; i < 8; i++) {
        long dr = static_cast<long>(c.r) - base[i].r;
        long dg = static_cast<long>(c.g) - base[i].g;
        long db = static_cast<long>(c.b) - base[i].b;
        long d = dr * dr + dg * dg + db * db;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Line helpers
// ---------------------------------------------------------------------------

size_t Line::width() const {
    size_t w = utf8::width(gutter);
    for (const Span& s : spans) w += utf8::width(s.text);
    return w;
}

std::string Line::plain() const {
    std::string out = gutter;
    for (const Span& s : spans) out += s.text;
    return out;
}

std::vector<Line> plain_lines(const std::string& text, size_t cols, Style style) {
    std::vector<Line> out;
    for (const std::string& l : utf8::wrap(utf8::sanitize(text), cols)) {
        Line line;
        line.spans.push_back({l, style});
        out.push_back(std::move(line));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Syntax highlighting
// ---------------------------------------------------------------------------

namespace {

struct LangSpec {
    std::set<std::string> keywords;
    std::set<std::string> types;
    std::set<std::string> constants;
    std::string line_comment;
    std::string block_open, block_close;
    bool dq_strings = true;
    bool sq_strings = true;
    bool backtick_strings = false;
    bool triple_quotes = false;
    bool preproc_hash = false;      // C-style #include
    bool dollar_vars = false;       // shell $VAR / ${VAR}
    bool key_colon = false;         // YAML-ish "key:" highlighting
};

const std::set<std::string> kCppKeywords = {
    "alignas","alignof","and","asm","auto","break","case","catch","class","concept",
    "const","consteval","constexpr","constinit","const_cast","continue","co_await",
    "co_return","co_yield","decltype","default","delete","do","dynamic_cast","else",
    "enum","explicit","export","extern","for","friend","goto","if","inline",
    "mutable","namespace","new","noexcept","not","operator","or","private",
    "protected","public","register","reinterpret_cast","requires","return",
    "sizeof","static","static_assert","static_cast","struct","switch","template",
    "this","thread_local","throw","try","typedef","typeid","typename","union",
    "using","virtual","volatile","while","xor","override","final",
};
const std::set<std::string> kCppTypes = {
    "bool","char","char8_t","char16_t","char32_t","double","float","int","long",
    "short","signed","unsigned","void","wchar_t","size_t","ssize_t","int8_t",
    "int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
    "string","vector","map","set","pair","optional","function","atomic","json",
    "FILE","pid_t","uintmax_t",
};
const std::set<std::string> kCppConstants = {
    "true","false","nullptr","NULL","EOF","stdin","stdout","stderr",
};

const std::set<std::string> kPyKeywords = {
    "and","as","assert","async","await","break","class","continue","def","del",
    "elif","else","except","finally","for","from","global","if","import","in",
    "is","lambda","nonlocal","not","or","pass","raise","return","try","while",
    "with","yield","match","case",
};
const std::set<std::string> kPyConstants = {"True","False","None","self","cls"};
const std::set<std::string> kPyTypes = {
    "int","str","float","bool","list","dict","set","tuple","bytes","object",
    "Exception","ValueError","TypeError","KeyError","OSError",
};

const std::set<std::string> kJsKeywords = {
    "async","await","break","case","catch","class","const","continue","debugger",
    "default","delete","do","else","export","extends","finally","for","function",
    "if","import","in","instanceof","let","new","of","return","static","super",
    "switch","this","throw","try","typeof","var","void","while","with","yield",
};
const std::set<std::string> kJsConstants = {"true","false","null","undefined","NaN"};
const std::set<std::string> kJsTypes = {
    "Array","Object","String","Number","Boolean","Promise","Map","Set","JSON",
    "Math","Date","RegExp","Error","Uint8Array",
};

const std::set<std::string> kShKeywords = {
    "if","then","else","elif","fi","for","while","until","do","done","case","esac",
    "function","in","select","time","return","exit","break","continue","local",
    "export","readonly","declare","set","unset","shift","trap","source",
};
const std::set<std::string> kShTypes = {
    "echo","printf","cd","pwd","test","read","eval","exec","mkdir","rm","cp","mv",
    "ln","cat","grep","sed","awk","find","sort","uniq","head","tail","wc","tr",
    "make","gmake","gcc","g++","clang","git","port","sudo","chmod","chown",
};

std::string canonical_lang(const std::string& raw) {
    std::string l = to_lower(trim(raw));
    // Strip anything after a space or comma in the info-string.
    size_t cut = l.find_first_of(" ,;");
    if (cut != std::string::npos) l = l.substr(0, cut);

    if (l == "c" || l == "h" || l == "cc" || l == "cpp" || l == "c++" ||
        l == "hpp" || l == "cxx" || l == "objc" || l == "objective-c" || l == "m")
        return "cpp";
    if (l == "py" || l == "python" || l == "python3") return "python";
    if (l == "js" || l == "javascript" || l == "mjs" || l == "ts" ||
        l == "typescript" || l == "jsx" || l == "tsx")
        return "js";
    if (l == "sh" || l == "bash" || l == "zsh" || l == "shell" || l == "console" ||
        l == "terminal")
        return "sh";
    if (l == "json") return "json";
    if (l == "yaml" || l == "yml") return "yaml";
    if (l == "make" || l == "makefile" || l == "mk") return "make";
    if (l == "diff" || l == "patch") return "diff";
    return l;
}

bool build_spec(const std::string& lang, LangSpec* out) {
    LangSpec s;
    if (lang == "cpp") {
        s.keywords = kCppKeywords; s.types = kCppTypes; s.constants = kCppConstants;
        s.line_comment = "//"; s.block_open = "/*"; s.block_close = "*/";
        s.preproc_hash = true;
    } else if (lang == "python") {
        s.keywords = kPyKeywords; s.types = kPyTypes; s.constants = kPyConstants;
        s.line_comment = "#"; s.triple_quotes = true;
    } else if (lang == "js") {
        s.keywords = kJsKeywords; s.types = kJsTypes; s.constants = kJsConstants;
        s.line_comment = "//"; s.block_open = "/*"; s.block_close = "*/";
        s.backtick_strings = true;
    } else if (lang == "sh") {
        s.keywords = kShKeywords; s.types = kShTypes;
        s.line_comment = "#"; s.dollar_vars = true; s.backtick_strings = true;
    } else if (lang == "json") {
        s.constants = {"true", "false", "null"};
        s.sq_strings = false; s.key_colon = true;
    } else if (lang == "yaml") {
        s.constants = {"true", "false", "null", "yes", "no", "on", "off", "~"};
        s.line_comment = "#"; s.key_colon = true;
    } else if (lang == "make") {
        s.keywords = {"ifeq","ifneq","ifdef","ifndef","else","endif","include",
                      "define","endef","export","unexport","override","vpath"};
        s.types = kShTypes;
        s.line_comment = "#"; s.dollar_vars = true;
    } else {
        return false;
    }
    *out = std::move(s);
    return true;
}

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Append text to a line, merging with the previous span when the style matches
// so the draw loop does fewer attribute changes.
void push_span(Line* line, const std::string& text, Style st) {
    if (text.empty()) return;
    if (!line->spans.empty() && line->spans.back().style == st)
        line->spans.back().text += text;
    else
        line->spans.push_back({text, st});
}

// Highlight one logical source line. `in_block_comment` and `in_triple` carry
// state across lines within the same code block.
Line highlight_line(const std::string& src, const LangSpec& spec,
                    bool* in_block_comment, std::string* in_triple) {
    Line out;
    size_t i = 0;
    const size_t n = src.size();

    // Continuation of a block comment from a previous line.
    if (*in_block_comment) {
        size_t end = spec.block_close.empty() ? std::string::npos
                                              : src.find(spec.block_close);
        if (end == std::string::npos) {
            push_span(&out, src, Style::Comment);
            return out;
        }
        size_t stop = end + spec.block_close.size();
        push_span(&out, src.substr(0, stop), Style::Comment);
        *in_block_comment = false;
        i = stop;
    }

    // Continuation of a triple-quoted string.
    if (!in_triple->empty()) {
        size_t end = src.find(*in_triple, i);
        if (end == std::string::npos) {
            push_span(&out, src.substr(i), Style::String);
            return out;
        }
        size_t stop = end + in_triple->size();
        push_span(&out, src.substr(i, stop - i), Style::String);
        in_triple->clear();
        i = stop;
    }

    // A whole-line preprocessor directive or a YAML/JSON key.
    if (spec.preproc_hash) {
        size_t ws = src.find_first_not_of(" \t", i);
        if (ws != std::string::npos && src[ws] == '#') {
            // Directive name, then let the rest fall through as normal tokens.
            size_t end = ws + 1;
            while (end < n && ident_char(src[end])) end++;
            push_span(&out, src.substr(i, end - i), Style::Preproc);
            i = end;
        }
    }

    while (i < n) {
        char c = src[i];

        // Comments
        if (!spec.line_comment.empty() &&
            src.compare(i, spec.line_comment.size(), spec.line_comment) == 0) {
            push_span(&out, src.substr(i), Style::Comment);
            return out;
        }
        if (!spec.block_open.empty() &&
            src.compare(i, spec.block_open.size(), spec.block_open) == 0) {
            size_t end = src.find(spec.block_close, i + spec.block_open.size());
            if (end == std::string::npos) {
                push_span(&out, src.substr(i), Style::Comment);
                *in_block_comment = true;
                return out;
            }
            size_t stop = end + spec.block_close.size();
            push_span(&out, src.substr(i, stop - i), Style::Comment);
            i = stop;
            continue;
        }

        // Triple-quoted strings
        if (spec.triple_quotes && (c == '"' || c == '\'') && i + 2 < n &&
            src[i + 1] == c && src[i + 2] == c) {
            std::string delim(3, c);
            size_t end = src.find(delim, i + 3);
            if (end == std::string::npos) {
                push_span(&out, src.substr(i), Style::String);
                *in_triple = delim;
                return out;
            }
            size_t stop = end + 3;
            push_span(&out, src.substr(i, stop - i), Style::String);
            i = stop;
            continue;
        }

        // Ordinary strings
        bool is_str_delim = (c == '"' && spec.dq_strings) ||
                            (c == '\'' && spec.sq_strings) ||
                            (c == '`' && spec.backtick_strings);
        if (is_str_delim) {
            size_t j = i + 1;
            while (j < n) {
                if (src[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (src[j] == c) { j++; break; }
                j++;
            }
            push_span(&out, src.substr(i, j - i), Style::String);
            i = j;
            continue;
        }

        // Shell / make variables
        if (spec.dollar_vars && c == '$') {
            size_t j = i + 1;
            if (j < n && (src[j] == '{' || src[j] == '(')) {
                char close = (src[j] == '{') ? '}' : ')';
                while (j < n && src[j] != close) j++;
                if (j < n) j++;
            } else {
                while (j < n && ident_char(src[j])) j++;
            }
            push_span(&out, src.substr(i, j - i), Style::Constant);
            i = j;
            continue;
        }

        // Numbers
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])) ||
                             src[j] == '.' || src[j] == 'x' || src[j] == 'X'))
                j++;
            push_span(&out, src.substr(i, j - i), Style::Number);
            i = j;
            continue;
        }

        // Identifiers
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && ident_char(src[j])) j++;
            std::string word = src.substr(i, j - i);

            Style st = Style::Plain;
            if (spec.keywords.count(word))       st = Style::Keyword;
            else if (spec.types.count(word))     st = Style::Type;
            else if (spec.constants.count(word)) st = Style::Constant;
            else {
                // A name immediately followed by '(' reads as a call.
                size_t k = j;
                while (k < n && src[k] == ' ') k++;
                if (k < n && src[k] == '(') st = Style::Function;
                // "key:" in JSON/YAML.
                else if (spec.key_colon && k < n && src[k] == ':') st = Style::Type;
            }
            push_span(&out, word, st);
            i = j;
            continue;
        }

        // Operators and punctuation
        if (std::ispunct(static_cast<unsigned char>(c))) {
            push_span(&out, std::string(1, c), Style::Operator);
            i++;
            continue;
        }

        push_span(&out, std::string(1, c), Style::CodeBg);
        i++;
    }
    return out;
}

// Diff blocks are highlighted by line prefix rather than tokenised.
std::vector<Line> highlight_diff(const std::string& code, size_t cols) {
    std::vector<Line> out;
    for (const std::string& raw : split(code, '\n')) {
        Style st = Style::CodeBg;
        if (starts_with(raw, "+++") || starts_with(raw, "---")) st = Style::Preproc;
        else if (starts_with(raw, "@@")) st = Style::Type;
        else if (starts_with(raw, "+")) st = Style::String;      // added: green
        else if (starts_with(raw, "-")) st = Style::ErrorText;   // removed: red
        else if (starts_with(raw, "diff ")) st = Style::Preproc;

        for (const std::string& l : utf8::wrap(utf8::sanitize(raw), cols)) {
            Line line;
            line.spans.push_back({l, st});
            out.push_back(std::move(line));
        }
    }
    return out;
}

} // namespace

bool language_supported(const std::string& lang) {
    std::string c = canonical_lang(lang);
    if (c == "diff") return true;
    LangSpec s;
    return build_spec(c, &s);
}

std::vector<Line> highlight(const std::string& code, const std::string& lang,
                            size_t cols) {
    std::string canon = canonical_lang(lang);
    if (canon == "diff") return highlight_diff(code, cols);

    LangSpec spec;
    if (!build_spec(canon, &spec))
        return plain_lines(code, cols, Style::CodeBg);

    std::vector<Line> out;
    bool in_block = false;
    std::string in_triple;

    for (const std::string& raw : split(code, '\n')) {
        std::string src = utf8::sanitize(raw);
        // Wrapping a highlighted line would split tokens, so highlight the
        // whole logical line and then break the resulting spans by width.
        Line full = highlight_line(src, spec, &in_block, &in_triple);

        if (full.width() <= cols) {
            out.push_back(std::move(full));
            continue;
        }

        Line cur;
        size_t used = 0;
        for (const Span& sp : full.spans) {
            std::string rest = sp.text;
            while (!rest.empty()) {
                size_t room = cols > used ? cols - used : 0;
                if (room == 0) {
                    out.push_back(std::move(cur));
                    cur = Line();
                    used = 0;
                    room = cols;
                }
                std::string piece = utf8::truncate_to_width(rest, room);
                if (piece.empty()) {          // a wide char that cannot fit
                    out.push_back(std::move(cur));
                    cur = Line();
                    used = 0;
                    continue;
                }
                push_span(&cur, piece, sp.style);
                used += utf8::width(piece);
                rest = rest.substr(piece.size());
            }
        }
        if (!cur.spans.empty()) out.push_back(std::move(cur));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------

namespace {

// Inline emphasis, inline code and links within one paragraph line.
std::vector<Span> inline_spans(const std::string& text) {
    std::vector<Span> out;
    std::string buf;
    size_t i = 0;
    const size_t n = text.size();

    auto flush = [&]() {
        if (!buf.empty()) { out.push_back({buf, Style::Plain}); buf.clear(); }
    };

    while (i < n) {
        char c = text[i];

        // `code`
        if (c == '`') {
            size_t end = text.find('`', i + 1);
            if (end != std::string::npos) {
                flush();
                out.push_back({text.substr(i + 1, end - i - 1), Style::InlineCode});
                i = end + 1;
                continue;
            }
        }
        // **bold** or __bold__
        if ((c == '*' || c == '_') && i + 1 < n && text[i + 1] == c) {
            std::string delim(2, c);
            size_t end = text.find(delim, i + 2);
            if (end != std::string::npos && end > i + 2) {
                flush();
                out.push_back({text.substr(i + 2, end - i - 2), Style::Bold});
                i = end + 2;
                continue;
            }
        }
        // *italic* -- require a non-space after the marker so "2 * 3" is safe
        if ((c == '*' || c == '_') && i + 1 < n && text[i + 1] != ' ') {
            size_t end = text.find(c, i + 1);
            if (end != std::string::npos && end > i + 1 && text[end - 1] != ' ') {
                flush();
                out.push_back({text.substr(i + 1, end - i - 1), Style::Italic});
                i = end + 1;
                continue;
            }
        }
        // [label](url)
        if (c == '[') {
            size_t close = text.find(']', i);
            if (close != std::string::npos && close + 1 < n && text[close + 1] == '(') {
                size_t paren = text.find(')', close + 2);
                if (paren != std::string::npos) {
                    flush();
                    out.push_back({text.substr(i + 1, close - i - 1), Style::Link});
                    i = paren + 1;
                    continue;
                }
            }
        }
        buf += c;
        i++;
    }
    flush();
    return out;
}

// Re-wrap a run of styled spans to `cols`, breaking at spaces where possible.
std::vector<Line> wrap_spans(const std::vector<Span>& spans, size_t cols,
                             const std::string& first_prefix,
                             const std::string& cont_prefix,
                             Style prefix_style) {
    std::vector<Line> out;
    size_t prefix_w = utf8::width(first_prefix);
    size_t avail = cols > prefix_w ? cols - prefix_w : 8;

    // Flatten to (char, style) then re-split; paragraphs are short enough that
    // this is not worth optimising further.
    struct Ch { std::string bytes; Style st; size_t w; };
    std::vector<Ch> chars;
    for (const Span& sp : spans) {
        size_t i = 0;
        while (i < sp.text.size()) {
            size_t start = i;
            uint32_t cp = utf8::decode(sp.text, &i);
            chars.push_back({sp.text.substr(start, i - start), sp.style,
                             static_cast<size_t>(utf8::cp_width(cp))});
        }
    }

    size_t pos = 0;
    bool first = true;
    while (pos < chars.size() || (first && chars.empty())) {
        size_t w = 0, i = pos;
        size_t last_space = std::string::npos;
        while (i < chars.size()) {
            if (w + chars[i].w > avail) break;
            if (chars[i].bytes == " ") last_space = i;
            w += chars[i].w;
            i++;
        }
        size_t end = i;
        if (i < chars.size() && last_space != std::string::npos && last_space > pos)
            end = last_space;

        Line line;
        line.gutter = first ? first_prefix : cont_prefix;
        line.gutter_style = prefix_style;
        for (size_t k = pos; k < end; k++)
            push_span(&line, chars[k].bytes, chars[k].st);
        out.push_back(std::move(line));

        pos = (end < chars.size() && chars[end].bytes == " ") ? end + 1 : end;
        first = false;
        if (chars.empty()) break;
        avail = cols > utf8::width(cont_prefix) ? cols - utf8::width(cont_prefix) : 8;
    }
    if (out.empty()) {
        Line l;
        l.gutter = first_prefix;
        l.gutter_style = prefix_style;
        out.push_back(std::move(l));
    }
    return out;
}

} // namespace

std::vector<Line> markdown(const std::string& text, size_t cols, bool unicode) {
    std::vector<Line> out;
    std::vector<std::string> lines = split(utf8::sanitize(text), '\n');

    const std::string bullet = unicode ? "\xE2\x80\xA2 " : "- ";      // •
    const std::string code_gutter = unicode ? "\xE2\x94\x82 " : "| "; // │
    const std::string quote_gutter = unicode ? "\xE2\x96\x8C " : "> "; // ▌

    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& raw = lines[i];
        std::string t = raw;
        std::string trimmed = trim(t);

        // Fenced code block
        if (starts_with(trimmed, "```") || starts_with(trimmed, "~~~")) {
            std::string fence = trimmed.substr(0, 3);
            std::string lang = trim(trimmed.substr(3));
            std::vector<std::string> body;
            i++;
            while (i < lines.size()) {
                std::string tt = trim(lines[i]);
                if (starts_with(tt, fence)) break;
                body.push_back(lines[i]);
                i++;
            }
            std::string code = join(body, "\n");
            size_t inner = cols > utf8::width(code_gutter)
                               ? cols - utf8::width(code_gutter) : cols;

            if (!lang.empty()) {
                Line hdr;
                hdr.gutter = code_gutter;
                hdr.spans.push_back({lang, Style::Dim});
                out.push_back(std::move(hdr));
            }
            for (Line& l : highlight(code, lang, inner)) {
                l.gutter = code_gutter;
                l.gutter_style = Style::Dim;
                out.push_back(std::move(l));
            }
            continue;
        }

        if (trimmed.empty()) {
            out.push_back(Line());
            continue;
        }

        // Horizontal rule
        if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
            Line l;
            std::string dash = unicode ? "\xE2\x94\x80" : "-";
            std::string s;
            for (size_t k = 0; k < cols && k < 78; k++) s += dash;
            l.spans.push_back({s, Style::Rule});
            out.push_back(std::move(l));
            continue;
        }

        // Headings
        if (trimmed[0] == '#') {
            size_t level = 0;
            while (level < trimmed.size() && trimmed[level] == '#') level++;
            std::string body = trim(trimmed.substr(level));
            Style st = (level == 1) ? Style::Heading1
                     : (level == 2) ? Style::Heading2 : Style::Heading3;
            std::vector<Span> spans{{body, st}};
            for (Line& l : wrap_spans(spans, cols, "", "", Style::Plain))
                out.push_back(std::move(l));
            continue;
        }

        // Blockquote
        if (trimmed[0] == '>') {
            std::string body = trim(trimmed.substr(1));
            std::vector<Span> spans = inline_spans(body);
            for (Span& s : spans) if (s.style == Style::Plain) s.style = Style::Quote;
            for (Line& l : wrap_spans(spans, cols, quote_gutter, quote_gutter,
                                      Style::Quote))
                out.push_back(std::move(l));
            continue;
        }

        // List item, preserving the original indentation
        {
            size_t lead = 0;
            while (lead < t.size() && t[lead] == ' ') lead++;
            std::string after = t.substr(lead);
            bool bullet_item = starts_with(after, "- ") || starts_with(after, "* ") ||
                               starts_with(after, "+ ");
            size_t num_len = 0;
            while (num_len < after.size() &&
                   std::isdigit(static_cast<unsigned char>(after[num_len])))
                num_len++;
            bool numbered = num_len > 0 && num_len + 1 < after.size() &&
                            (after[num_len] == '.' || after[num_len] == ')') &&
                            after[num_len + 1] == ' ';

            if (bullet_item || numbered) {
                std::string marker, body;
                if (bullet_item) {
                    marker = bullet;
                    body = after.substr(2);
                } else {
                    marker = after.substr(0, num_len + 1) + " ";
                    body = after.substr(num_len + 2);
                }
                std::string pad(lead, ' ');
                std::string first = pad + marker;
                std::string cont(utf8::width(first), ' ');
                for (Line& l : wrap_spans(inline_spans(trim(body)), cols, first, cont,
                                          Style::ListMarker))
                    out.push_back(std::move(l));
                continue;
            }
        }

        // Indented code (4 spaces) -- keep verbatim.
        if (starts_with(t, "    ")) {
            for (const std::string& l : utf8::wrap(t, cols)) {
                Line line;
                line.spans.push_back({l, Style::CodeBg});
                out.push_back(std::move(line));
            }
            continue;
        }

        // Ordinary paragraph line
        for (Line& l : wrap_spans(inline_spans(t), cols, "", "", Style::Plain))
            out.push_back(std::move(l));
    }
    return out;
}

} // namespace ppcode::render

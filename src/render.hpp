// render.hpp -- markdown and code rendering into styled spans.
//
// The TUI does not print raw text; it prints spans carrying a semantic Style.
// A palette then maps Style to whatever the terminal can actually do, from
// 24-bit RGB down to monochrome, so the same rendering code works on a modern
// terminal emulator and on the Leopard Terminal.app.
#pragma once

#include "common.hpp"

namespace ppcode::render {

// Semantic roles, not colours. The palette decides how each one looks.
enum class Style {
    Plain,
    Dim,
    Heading1, Heading2, Heading3,
    Bold, Italic,
    InlineCode,
    CodeBg,           // code-block text with no more specific class
    Quote,
    ListMarker,
    Link,
    Rule,
    // Roles used by the syntax highlighter
    Keyword, Type, Constant, String, Number, Comment, Preproc, Function, Operator,
    // Roles used by the chrome
    UserText, ToolName, ToolOutput, ErrorText, StatusText, Bar, Prompt,
    Count_
};

enum class ColorDepth { Mono, Ansi16, Ansi256, TrueColor };

// Inspect TERM, COLORTERM and what ncurses reports. Call after start_color().
ColorDepth detect_depth(int ncurses_colors, bool has_color);
std::string depth_name(ColorDepth d);

struct Rgb { uint8_t r = 0, g = 0, b = 0; };

// The colour and attributes a Style resolves to.
struct StyleDef {
    Rgb fg;
    bool bold = false;
    bool underline = false;
    bool dim = false;
    bool reverse = false;
    int ansi16 = -1;      // explicit 16-colour fallback (0-7), -1 to derive
};

const StyleDef& style_def(Style s);

// Nearest xterm-256 index for an RGB value.
int rgb_to_256(Rgb c);
// Nearest of the 8 base ANSI colours.
int rgb_to_16(Rgb c);

struct Span {
    std::string text;
    Style style = Style::Plain;
};

struct Line {
    std::vector<Span> spans;
    // Left gutter drawn before the spans, e.g. a code-block border.
    std::string gutter;
    Style gutter_style = Style::Dim;

    size_t width() const;
    std::string plain() const;
};

// Render markdown into wrapped, styled lines.
//   cols     -- available display columns
//   unicode  -- may use box drawing and typographic characters
std::vector<Line> markdown(const std::string& text, size_t cols, bool unicode);

// Highlight source code. `lang` is a markdown info-string such as "cpp" or
// "python"; unknown languages fall back to a neutral tokenizer.
std::vector<Line> highlight(const std::string& code, const std::string& lang,
                            size_t cols);

// True if we have a highlighter for this info-string.
bool language_supported(const std::string& lang);

// Split plain text into styled lines without interpreting markdown, for tool
// output and other verbatim content.
std::vector<Line> plain_lines(const std::string& text, size_t cols, Style style);

} // namespace ppcode::render

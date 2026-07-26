// mdparse.hpp -- markdown structure, independent of how it gets drawn.
//
// render.cpp already renders markdown, but it renders it *to a terminal*: it
// wraps to a column count and flattens everything to one Style per span. Both
// are wrong for a proportional-font NSTextView, which does its own wrapping and
// can carry bold and italic at the same time.
//
// So the structure is parsed here, into a neutral document model, and each
// front end decides how to draw it. This lives in a plain .cpp rather than in
// the Objective-C++ front end for two reasons: --selftest can reach it, and
// GCC's ObjC++ front end is a bad place to put intricate parsing (see
// knowledge/60-objcpp-gcc.md).
#pragma once

#include "common.hpp"

namespace ppcode::md {

enum class Block {
    Paragraph,
    Heading,     // level 1..6
    Code,        // lang + text, verbatim
    Bullet,      // level 0.. nesting depth
    Numbered,    // level 0.., marker as written ("3.")
    Rule,
    TableRow,    // cells; header marks the row above the delimiter
};

// Inline styles combine, so they are bits rather than an enumeration: **_x_**
// is Bold|Italic and there is no single value that means both.
enum : unsigned {
    StyleBold   = 1u << 0,
    StyleItalic = 1u << 1,
    StyleCode   = 1u << 2,
    StyleLink   = 1u << 3,
    StyleStrike = 1u << 4,
};

struct Run {
    std::string text;
    unsigned style = 0;
    std::string href;      // set when style has StyleLink
};

struct Node {
    Block kind = Block::Paragraph;

    // Heading level 1..6, or list nesting depth from 0.
    int level = 0;

    // How many blockquotes this node sits inside. Quoted content is parsed
    // recursively, so a list or a code block inside a quote comes out as the
    // list or code node it really is, with quote set.
    int quote = 0;

    std::string marker;    // Numbered: the marker as written, e.g. "3."
    std::string lang;      // Code: the fence info string, may be empty
    std::string text;      // Code: the body, verbatim, no trailing newline

    std::vector<Run> runs;                  // inline content of a text block
    std::vector<std::vector<Run>> cells;    // TableRow
    bool header = false;                    // TableRow: above the delimiter
    bool table_start = false;               // TableRow: opens its table
    bool table_end = false;                 // TableRow: closes its table
};

// Parse a whole document. Input need not be valid UTF-8; it is repaired.
std::vector<Node> parse(const std::string& text);

// Parse one run of inline markup. Exposed for testing and for callers that
// already have a single line.
std::vector<Run> parse_inline(const std::string& text);

// Byte length of the leading prefix of `text` that forms whole blocks and can
// therefore be rendered final.
//
// This is what makes streaming affordable. A delta arrives every few tokens; if
// the whole message were re-rendered each time, a G5 would spend all its time
// re-highlighting the same code block. Instead the caller renders the complete
// prefix once, keeps the trailing partial block as cheap plain text, and
// re-renders only when this function says another block has closed.
size_t complete_prefix(const std::string& text);

} // namespace ppcode::md

// utf8.hpp -- codepoint- and width-aware string handling.
//
// Everything the TUI does -- wrapping, truncating, positioning the cursor --
// has to work in terms of display columns, not bytes. Doing it bytewise splits
// multi-byte sequences and paints garbage on the screen.
#pragma once

#include "common.hpp"

namespace ppcode::utf8 {

// Number of bytes in the sequence starting at s[i], or 1 for invalid input so
// that iteration always makes progress.
size_t seq_len(const std::string& s, size_t i);

// Decode the codepoint at s[i]; advances *i. Returns U+FFFD for invalid bytes.
uint32_t decode(const std::string& s, size_t* i);

// Encode a codepoint as UTF-8.
std::string encode(uint32_t cp);

// Display columns occupied by one codepoint: 0 for combining marks and zero
// width characters, 2 for East Asian wide and most emoji, otherwise 1.
int cp_width(uint32_t cp);

// Total display width of a string.
size_t width(const std::string& s);

// Byte offsets of each codepoint boundary, plus a final entry equal to
// s.size(). Useful for cursor movement.
std::vector<size_t> boundaries(const std::string& s);

// Byte offset of the boundary at or before `byte`. Guarantees the result is a
// valid codepoint start.
size_t floor_boundary(const std::string& s, size_t byte);

// Move `n` codepoints from byte offset `from` (negative moves left), clamped.
size_t step(const std::string& s, size_t from, int n);

// Longest prefix whose display width is <= max_cols. Never splits a codepoint.
std::string truncate_to_width(const std::string& s, size_t max_cols);

// Width-aware line wrap, preserving the source text apart from the single space
// consumed at a break point. Mirrors common.hpp's wrap_text but in columns.
std::vector<std::string> wrap(const std::string& text, size_t cols);

// Width-aware middle elision.
std::string elide(const std::string& s, size_t max_cols);

// Replace control characters that would corrupt the display with visible
// placeholders. Tabs become spaces (tab_width columns).
std::string sanitize(const std::string& s, int tab_width = 4);

// True if the string is well-formed UTF-8.
bool valid(const std::string& s);

// Best-effort repair of invalid sequences, so a stray byte from a tool's output
// cannot wreck the screen.
std::string repair(const std::string& s);

} // namespace ppcode::utf8

// common.hpp -- shared types and small helpers.
#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace ppcode {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
std::vector<std::string> split(const std::string& s, char delim);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string to_lower(const std::string& s);

// Replace the first occurrence of `from` with `to`. Returns false if absent.
bool replace_first(std::string& s, const std::string& from, const std::string& to);

// Count occurrences of a (non-empty) needle.
size_t count_occurrences(const std::string& hay, const std::string& needle);

// Wrap `text` to `width` columns, honouring existing newlines. Never splits a
// word unless the word itself exceeds the width.
std::vector<std::string> wrap_text(const std::string& text, size_t width);

// Expand a leading "~" to $HOME.
std::string expand_user(const std::string& path);

// ---------------------------------------------------------------------------
// File I/O
//
// These use stdio rather than <fstream> deliberately. libcurl drags in
// CoreFoundation -> CoreServices -> Leopard's /usr/lib/libstdc++.6.dylib, so
// this process has two C++ runtimes. Darwin coalesces weak symbols across the
// whole process, so iostream/locale objects get allocated by one runtime and
// freed by the other, producing "Non-aligned pointer being freed" on every
// locale construction. Avoiding <iostream>/<fstream>/<sstream> sidesteps it
// completely. See README "Two libstdc++ runtimes".
// ---------------------------------------------------------------------------

bool read_file_text(const std::string& path, std::string* out, std::string* err);
bool write_file_text(const std::string& path, const std::string& data, std::string* err);

// Standard base64, no line breaks -- for data: URIs.
std::string base64_encode(const std::string& data);

// Elide the middle of a string so it fits in `max` characters.
std::string elide(const std::string& s, size_t max);

// ---------------------------------------------------------------------------
// JSON helpers -- OpenRouter and MCP payloads are full of optional fields, so
// these keep call sites free of repetitive contains()/is_null() checks.
// ---------------------------------------------------------------------------

std::string jstr(const json& j, const std::string& key, const std::string& def = "");
int64_t jint(const json& j, const std::string& key, int64_t def = 0);
double jnum(const json& j, const std::string& key, double def = 0.0);
bool jbool(const json& j, const std::string& key, bool def = false);
const json* jptr(const json& j, const std::string& key);

// Render a JSON value as a compact single-line string for display.
std::string json_preview(const json& j, size_t max = 160);

// ---------------------------------------------------------------------------
// Logging -- writes to a file, never to the terminal, because the TUI owns the
// screen. Enabled by PPCODE_LOG=<path>.
// ---------------------------------------------------------------------------

void log_init(const std::string& path);
void log_line(const std::string& msg);
bool log_enabled();

} // namespace ppcode

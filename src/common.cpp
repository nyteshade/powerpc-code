#include "common.hpp"

// No <iostream>/<fstream>/<sstream> here, on purpose -- see the note in
// common.hpp about the two libstdc++ runtimes in this process.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace ppcode {

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(delim, start);
        if (p == std::string::npos) break;
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    out.push_back(s.substr(start));
    // Match std::getline semantics: a trailing delimiter does not create a
    // final empty field.
    if (!out.empty() && out.back().empty() && !s.empty() && s.back() == delim)
        out.pop_back();
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string to_lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool replace_first(std::string& s, const std::string& from, const std::string& to) {
    size_t p = s.find(from);
    if (p == std::string::npos) return false;
    s.replace(p, from.size(), to);
    return true;
}

size_t count_occurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t n = 0, p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) {
        n++;
        p += needle.size();
    }
    return n;
}

// Wrapping preserves the source text byte for byte apart from the single space
// consumed at a break point. Collapsing runs of spaces would be fine for prose
// but destroys code indentation and aligned columns, which matters more here.
std::vector<std::string> wrap_text(const std::string& text, size_t width) {
    std::vector<std::string> out;
    if (width < 2) width = 2;

    for (const std::string& para : split(text, '\n')) {
        if (para.size() <= width) {
            out.push_back(para);
            continue;
        }
        size_t pos = 0;
        while (pos < para.size()) {
            size_t remaining = para.size() - pos;
            if (remaining <= width) {
                out.push_back(para.substr(pos));
                break;
            }
            // Prefer the last space that lets the line fit; fall back to a
            // hard break for a single over-long token.
            size_t brk = std::string::npos;
            for (size_t i = pos + width; i > pos; i--) {
                if (para[i] == ' ') { brk = i; break; }
            }
            if (brk == std::string::npos || brk == pos) {
                out.push_back(para.substr(pos, width));
                pos += width;
            } else {
                out.push_back(para.substr(pos, brk - pos));
                pos = brk + 1;      // swallow exactly the one break space
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// File I/O (stdio -- see header note)
// ---------------------------------------------------------------------------

bool read_file_text(const std::string& path, std::string* out, std::string* err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = std::string(std::strerror(errno)) + ": " + path;
        return false;
    }
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        if (err) *err = "read error: " + path;
        return false;
    }
    return true;
}

bool write_file_text(const std::string& path, const std::string& data, std::string* err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (err) *err = std::string(std::strerror(errno)) + ": " + path;
        return false;
    }
    size_t n = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    bool bad = (n != data.size()) || std::ferror(f) != 0;
    if (std::fclose(f) != 0) bad = true;
    if (bad) {
        if (err) *err = "write error: " + path;
        return false;
    }
    return true;
}

std::string base64_encode(const std::string& data) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t v = (static_cast<unsigned char>(data[i]) << 16) |
                     (static_cast<unsigned char>(data[i + 1]) << 8) |
                     static_cast<unsigned char>(data[i + 2]);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += tbl[v & 0x3F];
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t v = static_cast<unsigned char>(data[i]) << 16;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t v = (static_cast<unsigned char>(data[i]) << 16) |
                     (static_cast<unsigned char>(data[i + 1]) << 8);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::string expand_user(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

std::string elide(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    if (max <= 3) return s.substr(0, max);
    size_t head = (max - 3) / 2;
    size_t tail = max - 3 - head;
    return s.substr(0, head) + "..." + s.substr(s.size() - tail);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

const json* jptr(const json& j, const std::string& key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return nullptr;
    return &(*it);
}

std::string jstr(const json& j, const std::string& key, const std::string& def) {
    const json* v = jptr(j, key);
    if (!v) return def;
    if (v->is_string()) return v->get<std::string>();
    return v->dump();
}

int64_t jint(const json& j, const std::string& key, int64_t def) {
    const json* v = jptr(j, key);
    if (!v || !v->is_number()) return def;
    return v->get<int64_t>();
}

double jnum(const json& j, const std::string& key, double def) {
    const json* v = jptr(j, key);
    if (!v || !v->is_number()) return def;
    return v->get<double>();
}

bool jbool(const json& j, const std::string& key, bool def) {
    const json* v = jptr(j, key);
    if (!v || !v->is_boolean()) return def;
    return v->get<bool>();
}

std::string json_preview(const json& j, size_t max) {
    std::string s;
    if (j.is_string()) {
        s = j.get<std::string>();
    } else {
        s = j.dump();
    }
    // Collapse whitespace so it fits on one line.
    std::string flat;
    flat.reserve(s.size());
    bool ws = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!ws) {
                flat += ' ';
                ws = true;
            }
        } else {
            flat += c;
            ws = false;
        }
    }
    return elide(trim(flat), max);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

namespace {
std::mutex g_log_mu;
std::string g_log_path;
} // namespace

void log_init(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_log_path = path;
    if (path.empty()) return;
    if (FILE* f = std::fopen(path.c_str(), "a")) {
        std::fputs("\n=== ppcode session start ===\n", f);
        std::fclose(f);
    }
}

bool log_enabled() {
    std::lock_guard<std::mutex> lk(g_log_mu);
    return !g_log_path.empty();
}

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (g_log_path.empty()) return;
    FILE* f = std::fopen(g_log_path.c_str(), "a");
    if (!f) return;
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    std::fprintf(f, "[%s] %s\n", buf, msg.c_str());
    std::fclose(f);
}

} // namespace ppcode

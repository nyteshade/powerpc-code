#include "utf8.hpp"

#include <algorithm>

namespace ppcode::utf8 {

namespace {

constexpr uint32_t kReplacement = 0xFFFD;

bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

struct Range { uint32_t lo, hi; };

// Zero-width: combining marks, joiners, variation selectors.
const Range kZeroWidth[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x0711, 0x0711},
    {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0900, 0x0902}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},
    {0x0951, 0x0957}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E},
    {0x2060, 0x2064}, {0x20D0, 0x20F0}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

// Double-width: CJK, Hangul, and the emoji blocks that terminals render wide.
const Range kWide[] = {
    {0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6}, {0x1F300, 0x1F5FF}, {0x1F600, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

bool in_ranges(uint32_t cp, const Range* r, size_t n) {
    // Ranges are ordered, so a binary search is available, but n is small and
    // this is called per character during rendering -- keep it simple and
    // branch-predictable.
    for (size_t i = 0; i < n; i++) {
        if (cp < r[i].lo) return false;
        if (cp <= r[i].hi) return true;
    }
    return false;
}

} // namespace

size_t seq_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t need;
    if (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else return 1;                       // stray continuation or invalid lead

    if (i + need > s.size()) return 1;    // truncated
    for (size_t k = 1; k < need; k++)
        if (!is_continuation(static_cast<unsigned char>(s[i + k]))) return 1;
    return need;
}

uint32_t decode(const std::string& s, size_t* i) {
    if (*i >= s.size()) return 0;
    size_t n = seq_len(s, *i);
    unsigned char c = static_cast<unsigned char>(s[*i]);

    if (n == 1) {
        (*i)++;
        return (c < 0x80) ? c : kReplacement;
    }

    uint32_t cp = 0;
    if (n == 2)      cp = c & 0x1F;
    else if (n == 3) cp = c & 0x0F;
    else             cp = c & 0x07;

    for (size_t k = 1; k < n; k++)
        cp = (cp << 6) | (static_cast<unsigned char>(s[*i + k]) & 0x3F);

    *i += n;

    // Reject overlong encodings, surrogates, and out-of-range values.
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000))
        return kReplacement;
    if (cp >= 0xD800 && cp <= 0xDFFF) return kReplacement;
    if (cp > 0x10FFFF) return kReplacement;
    return cp;
}

std::string encode(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

int cp_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;   // control
    if (in_ranges(cp, kZeroWidth, sizeof(kZeroWidth) / sizeof(kZeroWidth[0]))) return 0;
    if (in_ranges(cp, kWide, sizeof(kWide) / sizeof(kWide[0]))) return 2;
    return 1;
}

size_t width(const std::string& s) {
    size_t i = 0, w = 0;
    while (i < s.size()) w += static_cast<size_t>(cp_width(decode(s, &i)));
    return w;
}

std::vector<size_t> boundaries(const std::string& s) {
    std::vector<size_t> out;
    size_t i = 0;
    while (i < s.size()) {
        out.push_back(i);
        i += seq_len(s, i);
    }
    out.push_back(s.size());
    return out;
}

size_t floor_boundary(const std::string& s, size_t byte) {
    if (byte >= s.size()) return s.size();
    while (byte > 0 && is_continuation(static_cast<unsigned char>(s[byte]))) byte--;
    return byte;
}

size_t step(const std::string& s, size_t from, int n) {
    size_t pos = floor_boundary(s, std::min(from, s.size()));
    if (n > 0) {
        for (int k = 0; k < n && pos < s.size(); k++) pos += seq_len(s, pos);
    } else if (n < 0) {
        for (int k = 0; k < -n && pos > 0; k++) {
            pos--;
            while (pos > 0 && is_continuation(static_cast<unsigned char>(s[pos]))) pos--;
        }
    }
    return pos;
}

std::string truncate_to_width(const std::string& s, size_t max_cols) {
    size_t i = 0, w = 0;
    while (i < s.size()) {
        size_t next = i;
        uint32_t cp = decode(s, &next);
        size_t cw = static_cast<size_t>(cp_width(cp));
        if (w + cw > max_cols) break;
        w += cw;
        i = next;
    }
    return s.substr(0, i);
}

std::vector<std::string> wrap(const std::string& text, size_t cols) {
    std::vector<std::string> out;
    if (cols < 2) cols = 2;

    for (const std::string& para : split(text, '\n')) {
        if (width(para) <= cols) {
            out.push_back(para);
            continue;
        }
        size_t pos = 0;
        while (pos < para.size()) {
            // Walk forward accumulating width, remembering the last space we
            // could break at.
            size_t i = pos, w = 0;
            size_t last_space = std::string::npos;
            while (i < para.size()) {
                size_t next = i;
                uint32_t cp = decode(para, &next);
                size_t cw = static_cast<size_t>(cp_width(cp));
                if (w + cw > cols) break;
                if (cp == ' ') last_space = i;
                w += cw;
                i = next;
            }
            if (i >= para.size()) {
                out.push_back(para.substr(pos));
                break;
            }
            if (last_space != std::string::npos && last_space > pos) {
                out.push_back(para.substr(pos, last_space - pos));
                pos = last_space + 1;
            } else {
                // One over-long token: hard break at the codepoint boundary.
                out.push_back(para.substr(pos, i - pos));
                pos = i;
            }
        }
    }
    return out;
}

std::string elide(const std::string& s, size_t max_cols) {
    if (width(s) <= max_cols) return s;
    if (max_cols <= 3) return truncate_to_width(s, max_cols);

    size_t keep = max_cols - 3;
    size_t head_cols = keep / 2;
    size_t tail_cols = keep - head_cols;

    std::string head = truncate_to_width(s, head_cols);

    // Walk backwards for the tail.
    size_t pos = s.size(), w = 0;
    while (pos > 0) {
        size_t prev = step(s, pos, -1);
        size_t tmp = prev;
        uint32_t cp = decode(s, &tmp);
        size_t cw = static_cast<size_t>(cp_width(cp));
        if (w + cw > tail_cols) break;
        w += cw;
        pos = prev;
    }
    return head + "..." + s.substr(pos);
}

std::string sanitize(const std::string& s, int tab_width) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = decode(s, &i);
        if (cp == '\t') {
            out.append(static_cast<size_t>(tab_width > 0 ? tab_width : 4), ' ');
        } else if (cp == '\n' || cp == '\r') {
            out += static_cast<char>(cp);
        } else if (cp < 0x20 || cp == 0x7F) {
            // Show C0 controls as a caret escape rather than letting the
            // terminal act on them.
            out += '^';
            out += static_cast<char>(cp == 0x7F ? '?' : ('@' + static_cast<char>(cp)));
        } else if (cp == kReplacement && (i - start) == 1) {
            out += "\xEF\xBF\xBD";   // invalid byte -> U+FFFD
        } else {
            out.append(s, start, i - start);
        }
    }
    return out;
}

bool valid(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t n = seq_len(s, i);
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (n == 1 && c >= 0x80) return false;
        size_t tmp = i;
        if (decode(s, &tmp) == kReplacement && !(c == 0xEF && n == 3)) return false;
        i += n;
    }
    return true;
}

std::string repair(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = decode(s, &i);
        if (cp == kReplacement) out += "\xEF\xBF\xBD";
        else out.append(s, start, i - start);
    }
    return out;
}

} // namespace ppcode::utf8

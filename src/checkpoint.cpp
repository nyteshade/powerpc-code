#include "checkpoint.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::checkpoint {

namespace {

// Longest common subsequence over lines. Source files are small enough for the
// quadratic table; anything enormous falls back to a whole-file replacement
// rather than spending seconds on a diff nobody will read.
constexpr size_t kMaxDiffLines = 4000;

enum class Op { Keep, Add, Del };
struct Edit { Op op; std::string text; };

std::vector<Edit> diff_lines(const std::vector<std::string>& a,
                             const std::vector<std::string>& b) {
    std::vector<Edit> out;
    const size_t n = a.size(), m = b.size();

    if (n > kMaxDiffLines || m > kMaxDiffLines) {
        for (const std::string& l : a) out.push_back({Op::Del, l});
        for (const std::string& l : b) out.push_back({Op::Add, l});
        return out;
    }

    // lcs[i][j] = length of the LCS of a[i..] and b[j..]
    std::vector<std::vector<uint32_t>> lcs(n + 1, std::vector<uint32_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            lcs[i][j] = (a[i] == b[j]) ? lcs[i + 1][j + 1] + 1
                                       : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }

    size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.push_back({Op::Keep, a[i]});
            i++;
            j++;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            out.push_back({Op::Del, a[i++]});
        } else {
            out.push_back({Op::Add, b[j++]});
        }
    }
    while (i < n) out.push_back({Op::Del, a[i++]});
    while (j < m) out.push_back({Op::Add, b[j++]});
    return out;
}

} // namespace

std::string diff_stat(const std::string& before, const std::string& after) {
    if (before == after) return "no change";
    std::vector<Edit> edits = diff_lines(split(before, '\n'), split(after, '\n'));
    int adds = 0, dels = 0;
    for (const Edit& e : edits) {
        if (e.op == Op::Add) adds++;
        else if (e.op == Op::Del) dels++;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "+%d -%d line%s", adds, dels,
                  (adds + dels) == 1 ? "" : "s");
    return buf;
}

std::string unified_diff(const std::string& before, const std::string& after,
                         const std::string& label, int context) {
    if (before == after) return "";

    std::vector<std::string> a = split(before, '\n');
    std::vector<std::string> b = split(after, '\n');
    std::vector<Edit> edits = diff_lines(a, b);

    // Mark which entries are near a change, so unchanged runs collapse.
    std::vector<bool> near(edits.size(), false);
    for (size_t i = 0; i < edits.size(); i++) {
        if (edits[i].op == Op::Keep) continue;
        size_t lo = (i > static_cast<size_t>(context)) ? i - context : 0;
        size_t hi = std::min(edits.size() - 1, i + static_cast<size_t>(context));
        for (size_t k = lo; k <= hi; k++) near[k] = true;
    }

    std::string out = "--- " + label + "\n+++ " + label + "\n";
    size_t old_line = 1, new_line = 1;
    bool in_hunk = false;
    size_t skipped = 0;

    for (size_t i = 0; i < edits.size(); i++) {
        const Edit& e = edits[i];
        if (!near[i]) {
            if (in_hunk) { in_hunk = false; }
            skipped++;
            if (e.op != Op::Add) old_line++;
            if (e.op != Op::Del) new_line++;
            continue;
        }
        if (!in_hunk) {
            char hdr[80];
            std::snprintf(hdr, sizeof(hdr), "@@ -%zu +%zu @@", old_line, new_line);
            if (skipped > 0) out += std::string(hdr) + "\n";
            else if (out.find("@@") == std::string::npos) out += std::string(hdr) + "\n";
            in_hunk = true;
            skipped = 0;
        }
        switch (e.op) {
            case Op::Keep: out += " " + e.text + "\n"; old_line++; new_line++; break;
            case Op::Del:  out += "-" + e.text + "\n"; old_line++;             break;
            case Op::Add:  out += "+" + e.text + "\n";             new_line++; break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------

Store& store() {
    static Store s;
    return s;
}

void Store::record_before(const std::string& path, const std::string& tool) {
    Entry e;
    e.path = path;
    e.tool = tool;
    e.at = static_cast<int64_t>(std::time(nullptr));
    std::error_code ec;
    e.existed_before = fs::is_regular_file(path, ec);
    if (e.existed_before) read_file_text(path, &e.before, nullptr);

    std::lock_guard<std::mutex> lk(mu_);
    pending_[path] = std::move(e);
}

std::string Store::record_after(const std::string& path) {
    Entry e;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_.find(path);
        if (it == pending_.end()) return "";
        e = it->second;
        pending_.erase(it);
    }

    read_file_text(path, &e.after, nullptr);
    if (e.before == e.after) return "";

    std::string label = e.path;
    std::string diff = unified_diff(e.before, e.after,
                                    e.existed_before ? label : label + " (new file)");

    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.push_back(std::move(e));
        // Keep the history bounded; whole file contents add up.
        if (entries_.size() > 200) entries_.erase(entries_.begin());
    }
    return diff;
}

int Store::undo(int count, std::string* report) {
    std::vector<Entry> to_revert;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < count && !entries_.empty(); i++) {
            to_revert.push_back(entries_.back());
            entries_.pop_back();
        }
    }
    if (to_revert.empty()) {
        if (report) *report = "nothing to undo";
        return 0;
    }

    int done = 0;
    std::string lines;
    for (const Entry& e : to_revert) {
        std::error_code ec;
        if (!e.existed_before) {
            // The tool created this file; undoing means removing it again.
            if (fs::remove(e.path, ec)) {
                lines += "  removed " + e.path + " (was created by " + e.tool + ")\n";
                done++;
            } else {
                lines += "  could not remove " + e.path + "\n";
            }
            continue;
        }
        std::string err;
        if (write_file_text(e.path, e.before, &err)) {
            lines += "  restored " + e.path + "  (" +
                     diff_stat(e.after, e.before) + ")\n";
            done++;
        } else {
            lines += "  could not restore " + e.path + ": " + err + "\n";
        }
    }
    if (report)
        *report = "undid " + std::to_string(done) + " change" +
                  (done == 1 ? "" : "s") + ":\n" + lines;
    return done;
}

std::vector<Entry> Store::history(int limit) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Entry> out;
    size_t start = 0;
    if (limit > 0 && entries_.size() > static_cast<size_t>(limit))
        start = entries_.size() - static_cast<size_t>(limit);
    for (size_t i = start; i < entries_.size(); i++) out.push_back(entries_[i]);
    std::reverse(out.begin(), out.end());   // newest first
    return out;
}

size_t Store::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

void Store::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
    pending_.clear();
}

} // namespace ppcode::checkpoint

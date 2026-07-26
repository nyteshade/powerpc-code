// checkpoint.hpp -- snapshots before every file mutation, and diffs of what
// changed.
//
// The model rewrites files. Most of the time that is what you asked for; when it
// is not, there needs to be a way back that does not depend on the project being
// in version control or on the change being small enough to spot. Every mutating
// tool records the previous contents first, so /undo can put them back.
//
// The same snapshot gives us the diff, which is what should be shown after an
// edit instead of "Replaced 1 occurrence" -- a count tells you nothing about
// whether the right thing changed.
#pragma once

#include "common.hpp"

#include <mutex>

namespace ppcode::checkpoint {

struct Entry {
    std::string path;
    std::string before;      // empty when the file did not exist
    std::string after;
    bool existed_before = false;
    std::string tool;        // which tool made the change
    int64_t at = 0;          // unix seconds
};

// A unified-style diff. `context` lines of unchanged text either side of each
// change. Returns an empty string when the two texts are identical.
std::string unified_diff(const std::string& before, const std::string& after,
                         const std::string& label, int context = 3);

// One-line summary: "+12 -3 lines".
std::string diff_stat(const std::string& before, const std::string& after);

class Store {
public:
    // Called by a mutating tool before it writes. Reads the current contents
    // itself so callers cannot forget.
    void record_before(const std::string& path, const std::string& tool);

    // Called after the write; completes the entry and returns the diff.
    std::string record_after(const std::string& path);

    // Revert the last `count` changes, newest first. Returns how many were
    // undone and describes them in `report`.
    int undo(int count, std::string* report);

    std::vector<Entry> history(int limit = 20) const;
    size_t size() const;
    void clear();

private:
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    // Between record_before and record_after for a path.
    std::map<std::string, Entry> pending_;
};

// The process-wide store, so tools and the front ends share one history.
Store& store();

} // namespace ppcode::checkpoint

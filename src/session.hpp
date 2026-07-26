// session.hpp -- persisting conversations, and compacting them when the context
// window fills.
//
// Two problems, one module.
//
// Continuity: work here spans hours. A build runs, the terminal gets closed, the
// machine is left alone overnight. Losing the conversation because ppcode exited
// is needless, so every session is written to disk as it goes and can be resumed
// by name or simply as "the last one".
//
// Compaction: a long session eventually exceeds the model's context. That is
// especially easy here because the system message alone is thousands of tokens
// of machine and platform context, and tool output accumulates fast. When the
// conversation approaches the limit, the older middle of it is replaced by a
// summary, keeping the system message and the most recent exchanges intact.
#pragma once

#include "common.hpp"
#include "openrouter.hpp"

namespace ppcode::session {

struct Meta {
    std::string id;             // filename stem
    std::string path;
    std::string title;          // first user message, elided
    std::string cwd;
    std::string model;
    int64_t updated_at = 0;
    int message_count = 0;
    double cost = 0.0;

    std::string age() const;    // "3m ago", "2h ago"
};

// Where sessions live: $XDG_DATA_HOME/ppcode/sessions or ~/.local/share/...
std::string sessions_dir();

// Newest first.
std::vector<Meta> list(int limit = 40);

// The most recently updated session, optionally restricted to one directory so
// --continue in a project resumes that project's work.
bool most_recent(const std::string& cwd_filter, Meta* out);

// Generate an id for a new session.
std::string new_id();

std::string path_for(const std::string& id);

// Delete sessions older than `days`, keeping at least `keep_min` of them.
int prune(int days, int keep_min);

// ---------------------------------------------------------------------------
// Compaction
// ---------------------------------------------------------------------------

struct CompactResult {
    bool ok = false;
    std::string error;
    std::string summary;
    int messages_before = 0;
    int messages_after = 0;
    int64_t tokens_before = 0;
    int64_t tokens_after = 0;
};

// Rough token estimate for a conversation, used to decide when to compact.
int64_t estimate_tokens(const std::vector<Message>& messages);

// Replace the older middle of `messages` with a summary produced by `client`.
// Keeps the system message and the last `keep_recent` messages untouched, and
// never splits a tool call from its result -- an assistant turn with tool_calls
// and the tool messages answering it must survive or be dropped together, or the
// next request is rejected.
CompactResult compact(Client& client, std::vector<Message>* messages,
                      int keep_recent = 6);

// True when the conversation is large enough relative to the window that it
// should be compacted before the next request.
bool should_compact(const std::vector<Message>& messages, int64_t context_tokens,
                    double threshold = 0.70);

} // namespace ppcode::session

// rag.hpp -- what goes into the index, and how it comes back out.
//
// vecstore.* is the database; this is the part that decides what a chunk is.
// That decision matters more than the retrieval algorithm: a chunk has to be
// one coherent idea, big enough to stand on its own when it arrives with no
// surrounding context, and small enough that retrieving it is not just
// retrieving the whole document again.
//
// Everything here works without an embedding model. Chunks are stored with no
// vector and searched lexically; when embeddings appear later they attach to
// the same rows.
#pragma once

#include "common.hpp"
#include "vecstore.hpp"

namespace ppcode { class ToolRegistry; }

namespace ppcode::rag {

// Collections are kept apart because they behave differently: conversations
// churn constantly and are chunked per exchange, reference material is static
// and chunked by heading. Searching one or all of them is then a filter rather
// than a separate index.
extern const char* kConversations;
extern const char* kKnowledge;
extern const char* kReference;

struct IndexStats {
    int documents = 0;
    int chunks = 0;
    int skipped = 0;         // unchanged since last time, or empty
    std::string error;
};

// Split prose into chunks on heading and paragraph boundaries, never mid
// sentence, targeting `target_chars` with `overlap_chars` carried between them.
//
// The overlap is not redundancy for its own sake: a passage split across a
// boundary is otherwise retrievable from neither half, because neither half
// contains the whole thought.
std::vector<vec::Store::Chunk> chunk_markdown(const std::string& text,
                                              const std::string& title,
                                              size_t target_chars = 1200,
                                              size_t overlap_chars = 160);

// One chunk per exchange -- a user message and everything the assistant said in
// reply. Splitting those apart is what makes a conversation archive useless to
// search: a question with no answer, or an answer with no question, is not
// something anyone wants back.
std::vector<vec::Store::Chunk> chunk_conversation(const json& session,
                                                  size_t target_chars = 1600);

// Index one saved session by id. Returns false and fills `error` on failure.
bool index_session(vec::Store& store, const std::string& session_id,
                   std::string* error);

// Index every saved session. Existing entries are replaced, so this is safe to
// re-run and is the recovery path if the index is ever deleted or corrupted --
// the JSON session files remain the source of truth.
IndexStats index_all_sessions(vec::Store& store,
                              const std::function<void(const std::string&)>& progress);

// Index a file or a directory tree of documents into `collection`. Markdown and
// plain text are read directly; anything else is skipped rather than indexed as
// binary noise.
IndexStats index_path(vec::Store& store, const std::string& path,
                      const std::string& collection,
                      const std::function<void(const std::string&)>& progress);

// Register search_knowledge on a registry. The store is opened lazily on first
// use and shared thereafter, so an installation that has never indexed anything
// pays nothing for the tool existing.
void add_tools(ppcode::ToolRegistry& registry);

// Render hits for a model to read: provenance first, because retrieved text
// with no attribution is how a guess gets laundered into an apparent fact.
std::string format_hits(const std::vector<vec::Hit>& hits, size_t max_chars = 4000);

} // namespace ppcode::rag

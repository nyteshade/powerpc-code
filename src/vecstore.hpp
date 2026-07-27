// vecstore.hpp -- the embedded database: SQLite, FTS5 and sqlite-vec.
//
// Both are vendored as amalgamations and compiled into the binary rather than
// linked from MacPorts, so a downloaded build needs nothing installed. That is
// not a detail: the whole point of the self-contained release is that someone
// can untar it onto a stock 10.5 machine and have it work.
//
// This wrapper exists so the rest of the codebase never sees a sqlite3* or has
// to remember to finalize a statement. It also keeps sqlite3.h out of every
// translation unit that merely wants to search.
#pragma once

#include "common.hpp"

namespace ppcode::vec {

// What the build actually linked, for --version and the environment probe.
std::string sqlite_version();
std::string sqlite_vec_version();

// True when the vector extension registered successfully.
bool vec_available();

// A search hit. `score` is comparable only within one search.
struct Hit {
    std::string doc_id;      // which conversation or document
    int64_t chunk_id = 0;
    std::string text;        // the chunk itself, for display and for context
    double score = 0.0;
    std::string source;      // "fts" or "vec", or "hybrid" once fused
};

// The store. One SQLite file, opened once, holding both the lexical index and
// the vectors, so a hybrid query is a single database away from either half.
class Store {
public:
    Store();
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Opens (creating if needed) and brings the schema up to date. Returns
    // false and sets *error on failure; the store is then unusable but safe.
    bool open(const std::string& path, std::string* error);
    void close();
    bool is_open() const;

    // Where the database lives by default: alongside the sessions, since it is
    // derived from them and should be deleted with them.
    static std::string default_path();

    // --- indexing ----------------------------------------------------------

    // Replace everything indexed for `doc_id`. Chunks are stored verbatim;
    // embeddings are optional, so the lexical half works with no model present.
    struct Chunk {
        std::string text;
        int ordinal = 0;                 // position within the document
        std::vector<float> embedding;    // empty when not embedded
    };

    bool put_document(const std::string& doc_id, const std::string& collection,
                      const std::vector<Chunk>& chunks, std::string* error);

    bool forget_document(const std::string& doc_id, std::string* error);

    // --- searching ---------------------------------------------------------

    // Lexical, via FTS5 and BM25. Needs no model and no network, which is why
    // it is the default.
    std::vector<Hit> search_text(const std::string& query, int limit,
                                 const std::string& collection = "") const;

    // Nearest neighbours by cosine distance. Empty when the vector half is
    // unavailable or nothing has been embedded.
    std::vector<Hit> search_vector(const std::vector<float>& query, int limit,
                                   const std::string& collection = "") const;

    // Both, fused by reciprocal rank. Degrades to whichever half can answer.
    std::vector<Hit> search(const std::string& query,
                            const std::vector<float>& query_embedding,
                            int limit,
                            const std::string& collection = "") const;

    // How many chunks are indexed, and how many carry an embedding.
    bool stats(int64_t* chunks, int64_t* embedded, std::string* error) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace ppcode::vec

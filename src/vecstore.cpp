#include "vecstore.hpp"

#include "session.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>

#include "sqlite3.h"
#include "sqlite-vec.h"

namespace ppcode::vec {

namespace {

// Reciprocal rank fusion constant. 60 is the value from the original paper and
// is not worth tuning until there is enough content to tune against: it damps
// the contribution of anything past the first page of either ranking.
const double kRrfK = 60.0;

// A prepared statement that finalizes itself. There is no exception handling in
// this codebase to unwind for us, and every early return below would otherwise
// need its own finalize.
class Stmt {
public:
    Stmt() = default;
    ~Stmt() { if (st_) sqlite3_finalize(st_); }

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool prepare(sqlite3* db, const std::string& sql) {
        if (st_) { sqlite3_finalize(st_); st_ = nullptr; }

        return sqlite3_prepare_v2(db, sql.c_str(), -1, &st_, nullptr) == SQLITE_OK;
    }

    sqlite3_stmt* get() const { return st_; }

private:
    sqlite3_stmt* st_ = nullptr;
};

bool exec(sqlite3* db, const std::string& sql, std::string* error) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK)
        return true;

    if (error) *error = err ? err : "sqlite error";
    if (err) sqlite3_free(err);

    return false;
}

std::string column_text(sqlite3_stmt* st, int i) {
    const unsigned char* p = sqlite3_column_text(st, i);
    int n = sqlite3_column_bytes(st, i);

    return p ? std::string(reinterpret_cast<const char*>(p),
                           static_cast<size_t>(n))
             : std::string();
}

// FTS5 treats a bare query as its own little language: bare "*" or an unbalanced
// quote is a syntax error, and punctuation from a pasted identifier is worse.
// Everything is quoted as a phrase, which is what a user typing into a search
// box means anyway.
std::string fts_quote(const std::string& q) {
    std::string out = "\"";
    for (size_t i = 0; i < q.size(); i++) {
        if (q[i] == '"') out += '"';   // doubled, per FTS5 quoting
        out += q[i];
    }
    out += "\"";

    return out;
}

} // namespace

std::string sqlite_version() { return sqlite3_libversion(); }
std::string sqlite_vec_version() { return SQLITE_VEC_VERSION; }

// ---------------------------------------------------------------------------

struct Store::Impl {
    sqlite3* db = nullptr;
    bool vec_ok = false;
    int embed_dim = 0;       // 0 until something has been embedded
};

Store::Store() : impl_(new Impl) {}

Store::~Store() {
    close();
    delete impl_;
}

bool Store::is_open() const { return impl_->db != nullptr; }

std::string Store::default_path() {
    // Beside the sessions: this index is derived from them, and deleting the
    // conversations should be able to take it with them.
    return (std::filesystem::path(session::sessions_dir()).parent_path() /
            "index.db").string();
}

void Store::close() {
    if (impl_->db) {
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
    }
}

bool Store::open(const std::string& path, std::string* error) {
    close();

    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);

    if (sqlite3_open(path.c_str(), &impl_->db) != SQLITE_OK) {
        if (error)
            *error = impl_->db ? sqlite3_errmsg(impl_->db) : "cannot open database";
        close();

        return false;
    }

    // Registered directly rather than through sqlite3_auto_extension, because
    // extension loading is compiled out -- this build has no way to load
    // anything at runtime, by design.
    char* verr = nullptr;
    impl_->vec_ok = (sqlite3_vec_init(impl_->db, &verr, nullptr) == SQLITE_OK);
    if (verr) sqlite3_free(verr);

    // WAL so the terminal tool and the application can both have it open. On a
    // spinning disk the synchronous=NORMAL pairing is the difference between an
    // index build taking minutes and taking hours.
    exec(impl_->db, "pragma journal_mode=WAL", nullptr);
    exec(impl_->db, "pragma synchronous=NORMAL", nullptr);
    exec(impl_->db, "pragma foreign_keys=ON", nullptr);

    const char* schema =
        "create table if not exists meta("
        "  key text primary key, value text) ;"
        "create table if not exists docs("
        "  doc_id text primary key,"
        "  collection text not null default '',"
        "  updated_at integer not null default 0) ;"
        "create table if not exists chunks("
        "  chunk_id integer primary key autoincrement,"
        "  doc_id text not null,"
        "  collection text not null default '',"
        "  ordinal integer not null default 0,"
        "  text text not null) ;"
        "create index if not exists chunks_doc on chunks(doc_id) ;"
        "create index if not exists chunks_coll on chunks(collection) ;"
        // A standalone FTS5 table with rowid kept equal to chunk_id, rather
        // than an external-content one. It costs a second copy of the text and
        // saves the trigger machinery that keeps external content in sync --
        // at this corpus size that is a good trade.
        "create virtual table if not exists chunks_fts using fts5("
        "  text, tokenize='porter unicode61') ;";

    if (!exec(impl_->db, schema, error)) {
        close();

        return false;
    }

    // Recover the embedding dimension, if this database has ever seen one.
    Stmt st;
    if (st.prepare(impl_->db, "select value from meta where key='embed_dim'") &&
        sqlite3_step(st.get()) == SQLITE_ROW) {
        impl_->embed_dim = sqlite3_column_int(st.get(), 0);
    }

    if (impl_->embed_dim > 0 && impl_->vec_ok) {
        // vec0 fixes the dimension at creation, so the table cannot exist until
        // the first embedding tells us how wide it is.
        std::string sql =
            "create virtual table if not exists chunks_vec using vec0("
            "  chunk_id integer primary key,"
            "  embedding float[" + std::to_string(impl_->embed_dim) +
            "] distance_metric=cosine)";
        exec(impl_->db, sql, nullptr);
    }

    return true;
}

bool vec_available() { return true; }

// ---------------------------------------------------------------------------

bool Store::forget_document(const std::string& doc_id, std::string* error) {
    if (!impl_->db) { if (error) *error = "database not open"; return false; }

    // The FTS and vector rows are keyed by chunk_id, so they have to go before
    // the chunks themselves do.
    Stmt sel;
    if (!sel.prepare(impl_->db, "select chunk_id from chunks where doc_id=?")) {
        if (error) *error = sqlite3_errmsg(impl_->db);

        return false;
    }
    sqlite3_bind_text(sel.get(), 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<int64_t> ids;
    while (sqlite3_step(sel.get()) == SQLITE_ROW)
        ids.push_back(sqlite3_column_int64(sel.get(), 0));

    for (size_t i = 0; i < ids.size(); i++) {
        Stmt d;
        if (d.prepare(impl_->db, "delete from chunks_fts where rowid=?")) {
            sqlite3_bind_int64(d.get(), 1, ids[i]);
            sqlite3_step(d.get());
        }

        if (impl_->vec_ok && impl_->embed_dim > 0) {
            Stmt v;
            if (v.prepare(impl_->db, "delete from chunks_vec where chunk_id=?")) {
                sqlite3_bind_int64(v.get(), 1, ids[i]);
                sqlite3_step(v.get());
            }
        }
    }

    Stmt dc;
    if (dc.prepare(impl_->db, "delete from chunks where doc_id=?")) {
        sqlite3_bind_text(dc.get(), 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(dc.get());
    }

    Stmt dd;
    if (dd.prepare(impl_->db, "delete from docs where doc_id=?")) {
        sqlite3_bind_text(dd.get(), 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(dd.get());
    }

    return true;
}

bool Store::put_document(const std::string& doc_id, const std::string& collection,
                         const std::vector<Chunk>& chunks, std::string* error) {
    if (!impl_->db) { if (error) *error = "database not open"; return false; }

    // Establish the vector table the first time an embedding shows up, and
    // refuse a second, different width rather than silently corrupting the
    // index: a mixed-dimension store returns nonsense distances.
    for (size_t i = 0; i < chunks.size(); i++) {
        if (chunks[i].embedding.empty()) continue;

        int dim = static_cast<int>(chunks[i].embedding.size());
        if (impl_->embed_dim == 0) {
            impl_->embed_dim = dim;

            Stmt m;
            if (m.prepare(impl_->db,
                          "insert or replace into meta(key,value) "
                          "values('embed_dim',?)")) {
                sqlite3_bind_int(m.get(), 1, dim);
                sqlite3_step(m.get());
            }

            if (impl_->vec_ok) {
                std::string sql =
                    "create virtual table if not exists chunks_vec using vec0("
                    "  chunk_id integer primary key,"
                    "  embedding float[" + std::to_string(dim) +
                    "] distance_metric=cosine)";
                if (!exec(impl_->db, sql, error)) return false;
            }
        }

        else if (dim != impl_->embed_dim) {
            if (error)
                *error = "embedding is " + std::to_string(dim) +
                         " wide but this index is " +
                         std::to_string(impl_->embed_dim) +
                         "; reindex after changing model";

            return false;
        }
    }

    if (!exec(impl_->db, "begin", error)) return false;

    if (!forget_document(doc_id, error)) {
        exec(impl_->db, "rollback", nullptr);

        return false;
    }

    {
        Stmt d;
        if (!d.prepare(impl_->db,
                       "insert or replace into docs(doc_id,collection,updated_at)"
                       " values(?,?,strftime('%s','now'))")) {
            if (error) *error = sqlite3_errmsg(impl_->db);
            exec(impl_->db, "rollback", nullptr);

            return false;
        }
        sqlite3_bind_text(d.get(), 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(d.get(), 2, collection.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(d.get());
    }

    for (size_t i = 0; i < chunks.size(); i++) {
        const Chunk& c = chunks[i];

        int64_t chunk_id = 0;
        {
            Stmt ins;
            if (!ins.prepare(impl_->db,
                             "insert into chunks(doc_id,collection,ordinal,text)"
                             " values(?,?,?,?)")) {
                if (error) *error = sqlite3_errmsg(impl_->db);
                exec(impl_->db, "rollback", nullptr);

                return false;
            }
            sqlite3_bind_text(ins.get(), 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.get(), 2, collection.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins.get(), 3, c.ordinal);
            sqlite3_bind_text(ins.get(), 4, c.text.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(ins.get()) != SQLITE_DONE) {
                if (error) *error = sqlite3_errmsg(impl_->db);
                exec(impl_->db, "rollback", nullptr);

                return false;
            }
            chunk_id = sqlite3_last_insert_rowid(impl_->db);
        }

        {
            Stmt f;
            if (f.prepare(impl_->db,
                          "insert into chunks_fts(rowid,text) values(?,?)")) {
                sqlite3_bind_int64(f.get(), 1, chunk_id);
                sqlite3_bind_text(f.get(), 2, c.text.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(f.get());
            }
        }

        if (!c.embedding.empty() && impl_->vec_ok) {
            Stmt v;
            if (v.prepare(impl_->db,
                          "insert into chunks_vec(chunk_id,embedding)"
                          " values(?,?)")) {
                sqlite3_bind_int64(v.get(), 1, chunk_id);
                // float32 straight out of the vector, which is what vec0 wants.
                sqlite3_bind_blob(v.get(), 2, c.embedding.data(),
                                  static_cast<int>(c.embedding.size() *
                                                   sizeof(float)),
                                  SQLITE_TRANSIENT);
                if (sqlite3_step(v.get()) != SQLITE_DONE) {
                    if (error) *error = sqlite3_errmsg(impl_->db);
                    exec(impl_->db, "rollback", nullptr);

                    return false;
                }
            }
        }
    }

    return exec(impl_->db, "commit", error);
}

// ---------------------------------------------------------------------------

std::vector<Hit> Store::search_text(const std::string& query, int limit,
                                    const std::string& collection) const {
    std::vector<Hit> out;
    if (!impl_->db || trim(query).empty()) return out;

    std::string sql =
        "select c.doc_id, c.chunk_id, c.text, bm25(chunks_fts) "
        "from chunks_fts f join chunks c on c.chunk_id = f.rowid "
        "where chunks_fts match ?";
    if (!collection.empty()) sql += " and c.collection = ?";
    // bm25() returns a negative score, better being more negative.
    sql += " order by bm25(chunks_fts) limit ?";

    Stmt st;
    if (!st.prepare(impl_->db, sql)) return out;

    std::string q = fts_quote(query);
    int arg = 1;
    sqlite3_bind_text(st.get(), arg++, q.c_str(), -1, SQLITE_TRANSIENT);
    if (!collection.empty())
        sqlite3_bind_text(st.get(), arg++, collection.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.get(), arg++, limit > 0 ? limit : 10);

    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        Hit h;
        h.doc_id = column_text(st.get(), 0);
        h.chunk_id = sqlite3_column_int64(st.get(), 1);
        h.text = column_text(st.get(), 2);
        h.score = -sqlite3_column_double(st.get(), 3);   // higher is better
        h.source = "fts";
        out.push_back(h);
    }

    return out;
}

std::vector<Hit> Store::search_vector(const std::vector<float>& query, int limit,
                                      const std::string& collection) const {
    std::vector<Hit> out;
    if (!impl_->db || !impl_->vec_ok || impl_->embed_dim == 0) return out;
    if (static_cast<int>(query.size()) != impl_->embed_dim) return out;

    // The knn constraint has to be applied inside the vec0 scan, so filtering by
    // collection happens after it -- ask for more than requested when a filter
    // is in play, or the filter eats the result set.
    int k = limit > 0 ? limit : 10;
    int fetch = collection.empty() ? k : k * 4;

    std::string sql =
        "select c.doc_id, c.chunk_id, c.text, v.distance "
        "from chunks_vec v join chunks c on c.chunk_id = v.chunk_id "
        "where v.embedding match ? and k = ?";
    if (!collection.empty()) sql += " and c.collection = ?";
    sql += " order by v.distance";

    Stmt st;
    if (!st.prepare(impl_->db, sql)) return out;

    sqlite3_bind_blob(st.get(), 1, query.data(),
                      static_cast<int>(query.size() * sizeof(float)),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(st.get(), 2, fetch);
    if (!collection.empty())
        sqlite3_bind_text(st.get(), 3, collection.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st.get()) == SQLITE_ROW &&
           static_cast<int>(out.size()) < k) {
        Hit h;
        h.doc_id = column_text(st.get(), 0);
        h.chunk_id = sqlite3_column_int64(st.get(), 1);
        h.text = column_text(st.get(), 2);
        // Cosine distance in [0,2]; flip so that higher is better everywhere.
        h.score = 1.0 - sqlite3_column_double(st.get(), 3);
        h.source = "vec";
        out.push_back(h);
    }

    return out;
}

std::vector<Hit> Store::search(const std::string& query,
                               const std::vector<float>& query_embedding,
                               int limit, const std::string& collection) const {
    int k = limit > 0 ? limit : 10;

    std::vector<Hit> lex = search_text(query, k * 2, collection);
    std::vector<Hit> sem = search_vector(query_embedding, k * 2, collection);

    if (sem.empty()) { lex.resize(std::min<size_t>(lex.size(), k)); return lex; }
    if (lex.empty()) { sem.resize(std::min<size_t>(sem.size(), k)); return sem; }

    // Reciprocal rank fusion. Ranks rather than scores, deliberately: BM25 and
    // cosine distance are not on comparable scales and any attempt to normalise
    // them into one number is arbitrary. Position in each list is meaningful,
    // and that is all this needs.
    std::map<int64_t, Hit> merged;
    std::map<int64_t, double> score;

    for (size_t i = 0; i < lex.size(); i++) {
        score[lex[i].chunk_id] += 1.0 / (kRrfK + static_cast<double>(i) + 1.0);
        merged[lex[i].chunk_id] = lex[i];
    }

    for (size_t i = 0; i < sem.size(); i++) {
        score[sem[i].chunk_id] += 1.0 / (kRrfK + static_cast<double>(i) + 1.0);
        if (merged.find(sem[i].chunk_id) == merged.end())
            merged[sem[i].chunk_id] = sem[i];
        else
            merged[sem[i].chunk_id].source = "hybrid";
    }

    std::vector<Hit> out;
    for (std::map<int64_t, Hit>::iterator it = merged.begin();
         it != merged.end(); ++it) {
        Hit h = it->second;
        h.score = score[it->first];
        out.push_back(h);
    }

    std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) {
        return a.score > b.score;
    });
    if (static_cast<int>(out.size()) > k) out.resize(k);

    return out;
}

std::vector<Store::Document> Store::list_documents(
    const std::string& collection) const {
    std::vector<Document> out;
    if (!impl_->db) return out;

    // Counting embedded chunks needs a join against the vector table, which
    // only exists once something has been embedded.
    bool have_vec = impl_->vec_ok && impl_->embed_dim > 0;

    std::string sql =
        "select d.doc_id, d.collection, d.updated_at, count(c.chunk_id) ";
    if (have_vec)
        sql += ", (select count(*) from chunks_vec v "
               "   join chunks cc on cc.chunk_id = v.chunk_id "
               "   where cc.doc_id = d.doc_id) ";
    else
        sql += ", 0 ";
    sql += "from docs d left join chunks c on c.doc_id = d.doc_id ";
    if (!collection.empty()) sql += "where d.collection = ? ";
    sql += "group by d.doc_id order by d.collection, d.doc_id";

    Stmt st;
    if (!st.prepare(impl_->db, sql)) return out;
    if (!collection.empty())
        sqlite3_bind_text(st.get(), 1, collection.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        Document d;
        d.doc_id = column_text(st.get(), 0);
        d.collection = column_text(st.get(), 1);
        d.updated_at = sqlite3_column_int64(st.get(), 2);
        d.chunks = sqlite3_column_int64(st.get(), 3);
        d.embedded = sqlite3_column_int64(st.get(), 4);
        out.push_back(d);
    }

    return out;
}

bool Store::stats(int64_t* chunks, int64_t* embedded, std::string* error) const {
    if (chunks) *chunks = 0;
    if (embedded) *embedded = 0;
    if (!impl_->db) { if (error) *error = "database not open"; return false; }

    Stmt a;
    if (a.prepare(impl_->db, "select count(*) from chunks") &&
        sqlite3_step(a.get()) == SQLITE_ROW && chunks)
        *chunks = sqlite3_column_int64(a.get(), 0);

    if (impl_->vec_ok && impl_->embed_dim > 0 && embedded) {
        Stmt b;
        if (b.prepare(impl_->db, "select count(*) from chunks_vec") &&
            sqlite3_step(b.get()) == SQLITE_ROW)
            *embedded = sqlite3_column_int64(b.get(), 0);
    }

    return true;
}

} // namespace ppcode::vec

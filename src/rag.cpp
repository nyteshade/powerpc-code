#include "rag.hpp"

#include "session.hpp"
#include "tools.hpp"

#include <algorithm>
#include <filesystem>

namespace ppcode::rag {

const char* kConversations = "conversations";
const char* kKnowledge = "knowledge";
const char* kReference = "reference";

namespace {

// A chunk carries its own provenance in its text. Retrieval hands the model a
// fragment with no surrounding document, so the fragment has to say where it
// came from -- otherwise the model cannot cite it and neither can the reader.
std::string with_heading(const std::string& title, const std::string& heading,
                         const std::string& body) {
    std::string out;
    if (!title.empty()) out += title;
    if (!heading.empty()) {
        if (!out.empty()) out += " \xE2\x80\xBA ";   // >
        out += heading;
    }
    if (!out.empty()) out += "\n\n";
    out += body;

    return out;
}

bool is_heading(const std::string& line) {
    std::string t = trim(line);

    return !t.empty() && t[0] == '#';
}

// Text files only. A PDF or an image indexed as bytes is noise that dilutes
// every real result, so anything unrecognised is skipped and counted.
bool indexable_extension(const std::filesystem::path& p) {
    std::string e = to_lower(p.extension().string());

    return e == ".md" || e == ".markdown" || e == ".txt" || e == ".text" ||
           e == ".rst" || e == ".org" || e == ".jsonl";
}

} // namespace

// ---------------------------------------------------------------------------

std::vector<vec::Store::Chunk> chunk_markdown(const std::string& text,
                                              const std::string& title,
                                              size_t target_chars,
                                              size_t overlap_chars) {
    std::vector<vec::Store::Chunk> out;
    std::vector<std::string> lines = split(text, '\n');

    std::string heading;      // most recent heading, carried into each chunk
    std::string buf;
    int ordinal = 0;

    auto flush = [&](const std::string& carry) {
        std::string body = trim(buf);
        if (body.empty()) { buf = carry; return; }

        vec::Store::Chunk c;
        c.ordinal = ordinal++;
        c.text = with_heading(title, heading, body);
        out.push_back(c);
        buf = carry;
    };

    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& line = lines[i];

        // A heading is a hard boundary: the text under it is about something
        // else, and merging across it produces a chunk about two things.
        if (is_heading(line)) {
            flush("");
            heading = trim(line);
            while (!heading.empty() && heading[0] == '#') heading.erase(0, 1);
            heading = trim(heading);
            continue;
        }

        buf += line;
        buf += "\n";

        if (buf.size() < target_chars) continue;

        // Break at the last paragraph gap so a chunk never ends mid sentence.
        size_t split_at = buf.rfind("\n\n");
        std::string carry;
        if (split_at != std::string::npos && split_at > target_chars / 3) {
            std::string tail = buf.substr(split_at);
            buf = buf.substr(0, split_at);
            carry = trim(tail);
            if (!carry.empty()) carry += "\n";
        }

        // Carry the end of this chunk into the next, so a thought spanning the
        // boundary is still retrievable from one of them.
        if (overlap_chars > 0 && buf.size() > overlap_chars) {
            std::string overlap = buf.substr(buf.size() - overlap_chars);
            size_t nl = overlap.find('\n');
            if (nl != std::string::npos) overlap = overlap.substr(nl + 1);
            carry = trim(overlap) + (carry.empty() ? "" : "\n" + carry);
            if (!carry.empty()) carry += "\n";
        }

        flush(carry);
    }

    flush("");

    return out;
}

std::vector<vec::Store::Chunk> chunk_conversation(const json& session,
                                                  size_t target_chars) {
    std::vector<vec::Store::Chunk> out;

    const json* msgs = jptr(session, "messages");
    if (!msgs || !msgs->is_array()) return out;

    std::string buf;
    int ordinal = 0;

    auto flush = [&]() {
        std::string body = trim(buf);
        buf.clear();
        if (body.empty()) return;

        vec::Store::Chunk c;
        c.ordinal = ordinal++;
        c.text = body;
        out.push_back(c);
    };

    for (const json& m : *msgs) {
        std::string role = jstr(m, "role");
        if (role == "system" || role == "tool") continue;   // not conversation

        // Content is either a string or an array of parts; only the text of a
        // part is worth indexing, since an image blob is not searchable prose.
        std::string content;
        const json* c = jptr(m, "content");
        if (c && c->is_string()) {
            content = c->get<std::string>();
        }

        else if (c && c->is_array()) {
            for (const json& part : *c) {
                std::string t = jstr(part, "text");
                if (!t.empty()) { content += t; content += "\n"; }
            }
        }

        content = trim(content);
        if (content.empty()) continue;

        // A user message opens a new exchange. Keeping the question with its
        // answer is the whole point: either alone is close to useless to search.
        if (role == "user" && !buf.empty() && buf.size() >= target_chars / 2) {
            flush();
        }

        buf += (role == "user" ? "Q: " : "A: ");
        buf += content;
        buf += "\n\n";

        if (buf.size() >= target_chars) flush();
    }

    flush();

    return out;
}

// ---------------------------------------------------------------------------

bool index_session(vec::Store& store, const std::string& session_id,
                   std::string* error) {
    std::string path = session::path_for(session_id);
    std::string text, err;
    if (!read_file_text(path, &text, &err)) {
        if (error) *error = err;

        return false;
    }

    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        if (error) *error = "session " + session_id + " is not valid JSON";

        return false;
    }

    std::vector<vec::Store::Chunk> chunks = chunk_conversation(j);
    if (chunks.empty()) {
        // Nothing to index is not a failure; an empty conversation is normal.
        store.forget_document(session_id, nullptr);

        return true;
    }

    return store.put_document(session_id, kConversations, chunks, error);
}

IndexStats index_all_sessions(
    vec::Store& store,
    const std::function<void(const std::string&)>& progress) {
    IndexStats stats;

    std::vector<session::Meta> all = session::list(100000);
    for (size_t i = 0; i < all.size(); i++) {
        std::string err;
        if (progress)
            progress("indexing " + all[i].id + " (" +
                     std::to_string(i + 1) + "/" + std::to_string(all.size()) + ")");

        if (index_session(store, all[i].id, &err)) {
            stats.documents++;
        }

        else {
            stats.skipped++;
            if (stats.error.empty()) stats.error = err;
        }
    }

    int64_t chunks = 0;
    store.stats(&chunks, nullptr, nullptr);
    stats.chunks = static_cast<int>(chunks);

    return stats;
}

IndexStats index_path(vec::Store& store, const std::string& path,
                      const std::string& collection,
                      const std::function<void(const std::string&)>& progress) {
    IndexStats stats;

    std::vector<std::filesystem::path> files;
    std::error_code ec;

    if (std::filesystem::is_directory(path, ec)) {
        std::filesystem::recursive_directory_iterator it(path, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && indexable_extension(it->path()))
                files.push_back(it->path());
            else if (it->is_regular_file(ec))
                stats.skipped++;
        }
    }

    else if (std::filesystem::is_regular_file(path, ec)) {
        if (indexable_extension(path)) files.push_back(path);
        else stats.skipped++;
    }

    else {
        stats.error = "no such file or directory: " + path;

        return stats;
    }

    std::sort(files.begin(), files.end());

    for (size_t i = 0; i < files.size(); i++) {
        std::string text, err;
        if (!read_file_text(files[i].string(), &text, &err)) {
            stats.skipped++;
            if (stats.error.empty()) stats.error = err;
            continue;
        }

        if (progress)
            progress("indexing " + files[i].filename().string() + " (" +
                     std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")");

        // The document id is the path, so re-indexing a changed file replaces
        // its chunks rather than adding a second copy.
        std::string doc_id = files[i].string();
        std::vector<vec::Store::Chunk> chunks =
            chunk_markdown(text, files[i].filename().string());

        if (chunks.empty()) { stats.skipped++; continue; }

        if (store.put_document(doc_id, collection, chunks, &err)) {
            stats.documents++;
            stats.chunks += static_cast<int>(chunks.size());
        }

        else {
            stats.skipped++;
            if (stats.error.empty()) stats.error = err;
        }
    }

    return stats;
}

// ---------------------------------------------------------------------------

std::string format_hits(const std::vector<vec::Hit>& hits, size_t max_chars) {
    if (hits.empty()) return "No matches.";

    std::string out;
    for (size_t i = 0; i < hits.size(); i++) {
        std::string entry = "[" + std::to_string(i + 1) + "] " + hits[i].doc_id;
        entry += "\n";
        entry += hits[i].text;
        entry += "\n\n";

        // Stop on a whole result rather than truncating one mid sentence: half
        // a retrieved passage is worse than one fewer passage.
        if (!out.empty() && out.size() + entry.size() > max_chars) {
            out += "(" + std::to_string(hits.size() - i) + " more omitted)\n";
            break;
        }

        out += entry;
    }

    return out;
}

// ---------------------------------------------------------------------------
// The tool
// ---------------------------------------------------------------------------

namespace {

// One store for the process, opened on demand. Opening is cheap but not free,
// and a session that never searches should not pay for it at all.
vec::Store& shared_store(bool* ok) {
    static vec::Store store;
    static bool tried = false;
    static bool opened = false;

    if (!tried) {
        tried = true;
        std::string err;
        opened = store.open(vec::Store::default_path(), &err);
    }

    if (ok) *ok = opened;

    return store;
}

} // namespace

void add_tools(ToolRegistry& registry) {
    Tool t;
    t.spec.name = "search_knowledge";
    t.spec.description =
        "Search indexed material: past conversations, the platform knowledge "
        "notes, and any reference documents that have been added. Use this when "
        "a question touches something specific to this machine, this project or "
        "this platform that you do not already know -- particularly Mac OS X "
        "10.5, PowerPC, Carbon and Cocoa details, and decisions taken earlier "
        "in other conversations. Results carry their source; cite it.";
    t.spec.parameters = json::parse(R"({
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "description": "What to look for. Literal identifiers and error text work well."
            },
            "collection": {
                "type": "string",
                "description": "Restrict to one of: conversations, knowledge, reference. Omit to search everything.",
                "enum": ["conversations", "knowledge", "reference"]
            },
            "limit": {
                "type": "integer",
                "description": "How many passages to return. Defaults to 5."
            }
        },
        "required": ["query"]
    })");
    t.kind = ToolKind::Read;
    t.source = "builtin";
    t.handler = [](const json& args, ToolContext&) -> ToolResult {
        std::string query = jstr(args, "query");
        if (trim(query).empty())
            return ToolResult::err("search_knowledge needs a query.");

        bool ok = false;
        vec::Store& store = shared_store(&ok);
        if (!ok)
            return ToolResult::err(
                "The search index is not available. Build it with /index.");

        std::string collection = jstr(args, "collection");
        int limit = static_cast<int>(jint(args, "limit", 5));
        if (limit < 1) limit = 5;
        if (limit > 20) limit = 20;

        // No embedding to offer yet, so this is the lexical half. The call site
        // does not change when vectors arrive.
        std::vector<vec::Hit> hits =
            store.search(query, std::vector<float>(), limit, collection);

        if (hits.empty())
            return ToolResult::ok(
                "No matches for \"" + query + "\"" +
                (collection.empty() ? "" : " in " + collection) + ".");

        return ToolResult::ok(format_hits(hits));
    };
    registry.add(std::move(t));
}

} // namespace ppcode::rag

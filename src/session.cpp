#include "session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ppcode::session {

std::string sessions_dir() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = std::string(home) + "/.local/share";
    else base = "/tmp";
    return base + "/ppcode/sessions";
}

std::string path_for(const std::string& id) {
    return sessions_dir() + "/" + id + ".json";
}

std::string new_id() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    // The pid disambiguates two sessions started in the same second.
    return std::string(buf) + "-" + std::to_string(static_cast<int>(getpid()));
}

std::string Meta::age() const {
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t d = now - updated_at;
    if (d < 0) d = 0;
    char buf[32];
    if (d < 60)        std::snprintf(buf, sizeof(buf), "%llds ago", (long long)d);
    else if (d < 3600) std::snprintf(buf, sizeof(buf), "%lldm ago", (long long)(d / 60));
    else if (d < 86400)std::snprintf(buf, sizeof(buf), "%lldh ago", (long long)(d / 3600));
    else               std::snprintf(buf, sizeof(buf), "%lldd ago", (long long)(d / 86400));
    return buf;
}

namespace {

bool read_meta(const fs::path& p, Meta* out) {
    std::string text;
    if (!read_file_text(p.string(), &text, nullptr)) return false;
    try {
        json j = json::parse(text);
        out->id = p.stem().string();
        out->path = p.string();
        out->cwd = jstr(j, "cwd");
        out->model = jstr(j, "model");
        out->title = jstr(j, "title");
        out->updated_at = jint(j, "updated_at");
        if (const json* u = jptr(j, "usage")) out->cost = jnum(*u, "cost");
        if (const json* m = jptr(j, "messages"); m && m->is_array())
            out->message_count = static_cast<int>(m->size());

        // Older files predate the stored title; derive one from the first user
        // turn so the picker is still useful for them.
        if (out->title.empty()) {
            if (const json* m = jptr(j, "messages"); m && m->is_array()) {
                for (const json& msg : *m) {
                    if (jstr(msg, "role") != "user") continue;
                    const json* c = jptr(msg, "content");
                    if (c && c->is_string()) {
                        out->title = elide(trim(c->get<std::string>()), 70);
                        break;
                    }
                }
            }
        }
        if (out->updated_at == 0) {
            struct stat st;
            if (stat(p.string().c_str(), &st) == 0)
                out->updated_at = static_cast<int64_t>(st.st_mtime);
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

std::vector<Meta> list(int limit) {
    std::vector<Meta> out;
    std::error_code ec;
    if (!fs::is_directory(sessions_dir(), ec)) return out;

    for (const auto& e : fs::directory_iterator(sessions_dir(), ec)) {
        if (!ends_with(e.path().string(), ".json")) continue;
        Meta m;
        if (read_meta(e.path(), &m)) out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(),
              [](const Meta& a, const Meta& b) { return a.updated_at > b.updated_at; });
    if (limit > 0 && static_cast<int>(out.size()) > limit)
        out.resize(static_cast<size_t>(limit));
    return out;
}

bool most_recent(const std::string& cwd_filter, Meta* out) {
    for (const Meta& m : list(200)) {
        if (!cwd_filter.empty() && m.cwd != cwd_filter) continue;
        if (m.message_count <= 1) continue;   // nothing but a system prompt
        *out = m;
        return true;
    }
    return false;
}

int prune(int days, int keep_min) {
    std::vector<Meta> all = list(0);
    if (static_cast<int>(all.size()) <= keep_min) return 0;

    int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) -
                     static_cast<int64_t>(days) * 86400;
    int removed = 0;
    for (size_t i = static_cast<size_t>(keep_min); i < all.size(); i++) {
        if (all[i].updated_at >= cutoff) continue;
        std::error_code ec;
        if (fs::remove(all[i].path, ec)) removed++;
    }
    return removed;
}

// ---------------------------------------------------------------------------
// Compaction
// ---------------------------------------------------------------------------

int64_t estimate_tokens(const std::vector<Message>& messages) {
    size_t chars = 0;
    for (const Message& m : messages) {
        chars += m.role.size() + m.content.size();
        for (const ContentPart& p : m.parts) {
            // A base64 image is enormous as text but costs far less as tokens;
            // count it at a flat rate rather than by its encoded length.
            if (p.type == ContentPart::Type::ImageUrl) chars += 4000;
            else chars += p.text.size();
        }
        for (const ToolCall& tc : m.tool_calls)
            chars += tc.name.size() + tc.arguments.size() + 16;
    }
    return static_cast<int64_t>(chars / 4 + 1);
}

bool should_compact(const std::vector<Message>& messages, int64_t context_tokens,
                    double threshold) {
    if (context_tokens <= 0) return false;
    return estimate_tokens(messages) >
           static_cast<int64_t>(static_cast<double>(context_tokens) * threshold);
}

namespace {

// A tool result cannot be separated from the assistant turn that requested it,
// so the cut has to land on a boundary where nothing is left dangling. Search
// backwards from `desired` for the first index where that holds.
size_t safe_cut(const std::vector<Message>& msgs, size_t desired) {
    if (desired >= msgs.size()) return msgs.size();
    for (size_t i = desired; i > 1; i--) {
        // Cutting before a tool message would orphan it from its call.
        if (msgs[i].role == "tool") continue;
        // Cutting immediately after an assistant turn that made tool calls
        // would drop the results it is waiting for.
        if (msgs[i - 1].role == "assistant" && !msgs[i - 1].tool_calls.empty())
            continue;
        return i;
    }
    return 1;   // keep only the system message
}

} // namespace

CompactResult compact(Client& client, std::vector<Message>* messages,
                      int keep_recent) {
    CompactResult res;
    if (!messages) {
        res.error = "no conversation";
        return res;
    }
    res.messages_before = static_cast<int>(messages->size());
    res.tokens_before = estimate_tokens(*messages);

    // Index 0 is the system message; keep it verbatim, it is the machine and
    // platform context and re-deriving it is expensive.
    const size_t sys = (!messages->empty() && (*messages)[0].role == "system") ? 1 : 0;
    if (messages->size() <= sys + static_cast<size_t>(keep_recent) + 2) {
        res.error = "conversation is too short to be worth compacting";
        return res;
    }

    size_t want_keep = messages->size() - static_cast<size_t>(keep_recent);
    size_t cut = safe_cut(*messages, want_keep);
    if (cut <= sys + 1) {
        res.error = "could not find a safe point to summarise up to";
        return res;
    }

    // Build a transcript of the part being replaced.
    std::string transcript;
    for (size_t i = sys; i < cut; i++) {
        const Message& m = (*messages)[i];
        std::string body = m.display_text();
        if (!m.tool_calls.empty()) {
            for (const ToolCall& tc : m.tool_calls)
                body += "\n[called " + tc.name + " " +
                        json_preview(tc.arguments, 200) + "]";
        }
        if (m.role == "tool") body = "[result of " + m.name + "] " + body;
        transcript += m.role + ": " + elide(body, 4000) + "\n\n";
    }

    std::vector<Message> ask;
    ask.push_back(Message::system_msg(
        "You are compacting a coding session so that work can continue in a "
        "smaller context. Produce a summary that a fresh assistant could pick up "
        "from without re-reading anything."));
    ask.push_back(Message::user(
        "Summarise the conversation below. Include, in this order:\n"
        "\n"
        "1. What the user is ultimately trying to achieve.\n"
        "2. What has been done so far, with exact file paths and identifiers.\n"
        "3. Decisions made and the reasons, so they are not revisited.\n"
        "4. What is currently broken or unresolved.\n"
        "5. The immediate next step.\n"
        "\n"
        "Be specific: names, paths, line numbers, error text. Omit pleasantries "
        "and anything already superseded. This replaces the transcript, so "
        "anything you leave out is lost.\n"
        "\n"
        "----- transcript -----\n" + transcript));

    ChatResult cr = client.chat(ask, {});
    if (!cr.ok) {
        res.error = "could not summarise: " + cr.error;
        return res;
    }
    res.summary = trim(cr.message.content);
    if (res.summary.empty()) {
        res.error = "the summary came back empty";
        return res;
    }

    std::vector<Message> rebuilt;
    if (sys) rebuilt.push_back((*messages)[0]);
    rebuilt.push_back(Message::user(
        "[Earlier conversation, compacted to save context]\n\n" + res.summary));
    rebuilt.push_back(Message::assistant(
        "Understood. I have the summary of the earlier work and will continue "
        "from there."));
    for (size_t i = cut; i < messages->size(); i++) rebuilt.push_back((*messages)[i]);

    *messages = std::move(rebuilt);
    res.messages_after = static_cast<int>(messages->size());
    res.tokens_after = estimate_tokens(*messages);
    res.ok = true;
    return res;
}

} // namespace ppcode::session

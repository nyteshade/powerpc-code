// ui.cpp -- ncurses front end.
//
// Threading: ncurses is not thread-safe, so only the main thread touches it.
// The agent runs on a worker thread and communicates purely by pushing events
// onto a queue. Tool approval reverses the direction -- the worker blocks on a
// condition variable while the main thread collects the answer.
#include "ui.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <ncurses.h>
#include <unistd.h>

namespace ppcode {
namespace {

// ---------------------------------------------------------------------------
// Transcript model
// ---------------------------------------------------------------------------

enum class Kind { User, Assistant, Tool, ToolOutput, Status, Error, Info, Reasoning };

struct Entry {
    Kind kind;
    std::string text;
};

// Colour pairs.
enum {
    CP_USER = 1, CP_ASSIST, CP_TOOL, CP_ERROR, CP_STATUS, CP_DIM, CP_PROMPT, CP_BAR
};

int attr_for(Kind k, bool color) {
    if (!color) return k == Kind::Error ? A_BOLD : A_NORMAL;
    switch (k) {
        case Kind::User:       return COLOR_PAIR(CP_USER) | A_BOLD;
        case Kind::Assistant:  return COLOR_PAIR(CP_ASSIST);
        case Kind::Tool:       return COLOR_PAIR(CP_TOOL);
        case Kind::ToolOutput: return COLOR_PAIR(CP_DIM);
        case Kind::Status:     return COLOR_PAIR(CP_STATUS);
        case Kind::Error:      return COLOR_PAIR(CP_ERROR) | A_BOLD;
        case Kind::Reasoning:  return COLOR_PAIR(CP_DIM) | A_DIM;
        case Kind::Info:       return COLOR_PAIR(CP_STATUS);
    }
    return A_NORMAL;
}

// ---------------------------------------------------------------------------
// Worker -> UI events
// ---------------------------------------------------------------------------

struct Event {
    enum Type { Text, Reasoning, Status, Error, ToolStart, ToolDone, Done, Approval } type;
    std::string a, b;
    bool flag = false;
};

class EventQueue {
public:
    void push(Event e) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(e));
        }
        cv_.notify_one();
    }
    bool try_pop(Event* out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.empty()) return false;
        *out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Event> q_;
};

// The worker parks here while the user decides.
struct ApprovalGate {
    std::mutex mu;
    std::condition_variable cv;
    bool pending = false;
    bool answered = false;
    bool allowed = false;
    std::string tool_name;
    ToolPreview preview;
};

// ---------------------------------------------------------------------------
// Input editor: a string with embedded newlines plus a cursor.
// ---------------------------------------------------------------------------

struct Editor {
    std::string text;
    size_t cursor = 0;
    std::vector<std::string> history;
    int hist_pos = -1;      // -1 == editing a fresh line
    std::string stash;      // the in-progress line while browsing history

    void insert(char c) { text.insert(cursor++, 1, c); }
    void insert_str(const std::string& s) { text.insert(cursor, s); cursor += s.size(); }

    void backspace() {
        if (cursor == 0) return;
        text.erase(cursor - 1, 1);
        cursor--;
    }
    void del() { if (cursor < text.size()) text.erase(cursor, 1); }
    void left()  { if (cursor > 0) cursor--; }
    void right() { if (cursor < text.size()) cursor++; }

    size_t line_start(size_t pos) const {
        size_t p = text.rfind('\n', pos ? pos - 1 : 0);
        if (pos == 0) return 0;
        return (p == std::string::npos) ? 0 : p + 1;
    }
    void home() { cursor = line_start(cursor); }
    void end() {
        size_t p = text.find('\n', cursor);
        cursor = (p == std::string::npos) ? text.size() : p;
    }
    void kill_to_end() {
        size_t p = text.find('\n', cursor);
        size_t e = (p == std::string::npos) ? text.size() : p;
        text.erase(cursor, e - cursor);
    }
    void clear() { text.clear(); cursor = 0; }

    void push_history(const std::string& s) {
        if (s.empty()) return;
        if (!history.empty() && history.back() == s) return;
        history.push_back(s);
        if (history.size() > 200) history.erase(history.begin());
    }
    void hist_prev() {
        if (history.empty()) return;
        if (hist_pos == -1) { stash = text; hist_pos = static_cast<int>(history.size()); }
        if (hist_pos > 0) hist_pos--;
        text = history[static_cast<size_t>(hist_pos)];
        cursor = text.size();
    }
    void hist_next() {
        if (hist_pos == -1) return;
        hist_pos++;
        if (hist_pos >= static_cast<int>(history.size())) {
            hist_pos = -1;
            text = stash;
        } else {
            text = history[static_cast<size_t>(hist_pos)];
        }
        cursor = text.size();
    }
};

// ---------------------------------------------------------------------------
// The UI itself
// ---------------------------------------------------------------------------

class Tui {
public:
    Tui(Agent& agent, Client& client, ToolRegistry& tools, Config cfg,
        mcp::Manager* mcp)
        : agent_(agent), client_(client), tools_(tools), cfg_(std::move(cfg)),
          mcp_(mcp) {}

    int run();

private:
    Agent& agent_;
    Client& client_;
    ToolRegistry& tools_;
    Config cfg_;
    mcp::Manager* mcp_ = nullptr;

    std::vector<Entry> entries_;
    EventQueue queue_;
    ApprovalGate gate_;

    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> quit_{false};

    Editor ed_;
    int scroll_ = 0;            // lines scrolled up from the bottom
    bool follow_ = true;
    std::string status_text_;
    int spinner_ = 0;
    Usage session_usage_;
    int last_rounds_ = 0;

    // Cached wrap of the transcript.
    std::vector<std::pair<std::string, int>> wrapped_;
    bool dirty_ = true;
    int wrapped_width_ = -1;

    bool color_ = false;
    bool approving_ = false;

    void add(Kind k, const std::string& text);
    void append_stream(Kind k, const std::string& delta);
    void rewrap(int width);
    void draw();
    void draw_transcript(int top, int height, int width);
    void draw_status(int row, int width);
    void draw_input(int top, int height, int width);
    int input_rows(int width) const;

    void handle_key(int ch);
    void submit();
    bool handle_slash(const std::string& line);
    void start_turn(const std::string& text);
    void pump_events();
    void finish_worker();

    std::string glyph(const char* uni, const char* ascii) const {
        return cfg_.unicode ? uni : ascii;
    }
};

void Tui::add(Kind k, const std::string& text) {
    entries_.push_back({k, text});
    dirty_ = true;
    if (follow_) scroll_ = 0;
}

// Streaming text lands in the trailing entry if it is the same kind, so the
// assistant's reply grows in place rather than one entry per token.
void Tui::append_stream(Kind k, const std::string& delta) {
    if (!entries_.empty() && entries_.back().kind == k) {
        entries_.back().text += delta;
    } else {
        entries_.push_back({k, delta});
    }
    dirty_ = true;
    if (follow_) scroll_ = 0;
}

void Tui::rewrap(int width) {
    if (!dirty_ && wrapped_width_ == width) return;
    wrapped_.clear();
    wrapped_width_ = width;

    for (const Entry& e : entries_) {
        int at = attr_for(e.kind, color_);
        std::string prefix;
        switch (e.kind) {
            case Kind::User:       prefix = glyph("› ", "> "); break;
            case Kind::Tool:       prefix = glyph("● ", "* "); break;
            case Kind::ToolOutput: prefix = "    "; break;
            case Kind::Error:      prefix = "! "; break;
            case Kind::Status:     prefix = "  "; break;
            case Kind::Info:       prefix = "  "; break;
            default:               prefix = ""; break;
        }
        int avail = width - static_cast<int>(prefix.size());
        if (avail < 8) avail = 8;

        std::vector<std::string> lines = wrap_text(e.text, static_cast<size_t>(avail));
        for (size_t i = 0; i < lines.size(); i++) {
            std::string ind = (i == 0) ? prefix : std::string(prefix.size(), ' ');
            wrapped_.emplace_back(ind + lines[i], at);
        }
        // A blank line after each block, except between a tool and its output.
        wrapped_.emplace_back("", A_NORMAL);
    }
    dirty_ = false;
}

int Tui::input_rows(int width) const {
    int avail = width - 2;
    if (avail < 8) avail = 8;
    std::vector<std::string> lines = wrap_text(ed_.text.empty() ? " " : ed_.text,
                                               static_cast<size_t>(avail));
    int n = static_cast<int>(lines.size());
    return std::min(n, 8);
}

void Tui::draw_transcript(int top, int height, int width) {
    rewrap(width);
    int total = static_cast<int>(wrapped_.size());
    int max_scroll = std::max(0, total - height);
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    if (scroll_ < 0) scroll_ = 0;

    int start = std::max(0, total - height - scroll_);
    for (int r = 0; r < height; r++) {
        int idx = start + r;
        move(top + r, 0);
        clrtoeol();
        if (idx < 0 || idx >= total) continue;
        const auto& [line, at] = wrapped_[static_cast<size_t>(idx)];
        attron(at);
        mvaddnstr(top + r, 0, line.c_str(), width);
        attroff(at);
    }
}

void Tui::draw_status(int row, int width) {
    std::string left;
    if (approving_) {
        left = "APPROVE? y=yes  n=no  a=allow all this session  ESC=no";
    } else if (busy_.load()) {
        static const char* frames = "|/-\\";
        left = std::string(1, frames[spinner_ % 4]) + " " +
               (status_text_.empty() ? "working" : status_text_) +
               "   (Ctrl+C cancels)";
    } else {
        left = status_text_.empty() ? "ready" : status_text_;
    }

    char right[256];
    std::snprintf(right, sizeof(right), "%s  %lldtok  $%.4f",
                  elide(cfg_.model, 34).c_str(),
                  static_cast<long long>(session_usage_.total_tokens),
                  session_usage_.cost);

    std::string bar = left;
    int rlen = static_cast<int>(std::strlen(right));
    int pad = width - static_cast<int>(bar.size()) - rlen - 1;
    if (pad < 1) {
        bar = elide(bar, static_cast<size_t>(std::max(0, width - rlen - 2)));
        pad = width - static_cast<int>(bar.size()) - rlen - 1;
        if (pad < 1) pad = 1;
    }
    bar += std::string(static_cast<size_t>(pad), ' ');
    bar += right;

    int at = color_ ? COLOR_PAIR(CP_BAR) : A_REVERSE;
    attron(at);
    move(row, 0);
    clrtoeol();
    mvaddnstr(row, 0, bar.c_str(), width);
    attroff(at);
}

void Tui::draw_input(int top, int height, int width) {
    int at = color_ ? COLOR_PAIR(CP_PROMPT) : A_NORMAL;
    std::string prompt = glyph("❯ ", "> ");

    int avail = width - static_cast<int>(prompt.size());
    if (avail < 8) avail = 8;

    std::vector<std::string> lines =
        wrap_text(ed_.text, static_cast<size_t>(avail));
    if (lines.empty()) lines.push_back("");

    // Where is the cursor in wrapped coordinates? wrap_text can rebreak on
    // spaces, so walk the text counting printable positions instead of
    // assuming a simple division.
    int cur_row = 0, cur_col = 0;
    {
        size_t remaining = ed_.cursor;
        size_t consumed = 0;
        for (size_t i = 0; i < lines.size(); i++) {
            size_t len = lines[i].size();
            // +1 for the break (newline or the space that was folded away)
            size_t take = std::min(remaining - std::min(remaining, consumed), len);
            if (consumed + len >= ed_.cursor) {
                cur_row = static_cast<int>(i);
                cur_col = static_cast<int>(ed_.cursor - consumed);
                if (cur_col > static_cast<int>(len)) cur_col = static_cast<int>(len);
                break;
            }
            consumed += len + 1;
            cur_row = static_cast<int>(i);
            cur_col = static_cast<int>(len);
            (void)take;
        }
    }

    int first = std::max(0, cur_row - height + 1);
    for (int r = 0; r < height; r++) {
        move(top + r, 0);
        clrtoeol();
        size_t idx = static_cast<size_t>(first + r);
        if (idx >= lines.size()) continue;
        attron(at);
        if (r == 0 || idx == 0) mvaddstr(top + r, 0, prompt.c_str());
        else mvaddstr(top + r, 0, std::string(prompt.size(), ' ').c_str());
        attroff(at);
        mvaddnstr(top + r, static_cast<int>(prompt.size()),
                  lines[idx].c_str(), avail);
    }

    int scr_row = top + (cur_row - first);
    int scr_col = static_cast<int>(prompt.size()) + cur_col;
    if (scr_row >= top && scr_row < top + height)
        move(scr_row, std::min(scr_col, width - 1));
}

void Tui::draw() {
    int h, w;
    getmaxyx(stdscr, h, w);
    if (h < 6 || w < 24) {
        erase();
        mvaddstr(0, 0, "terminal too small");
        refresh();
        return;
    }

    // Rows: transcript [0, trans_h), status bar at trans_h, input below.
    int in_rows = input_rows(w);
    if (in_rows > h - 3) in_rows = std::max(1, h - 3);
    int trans_h = h - in_rows - 1;
    if (trans_h < 1) trans_h = 1;

    draw_transcript(0, trans_h, w);
    draw_status(trans_h, w);
    draw_input(trans_h + 1, in_rows, w);
    refresh();
}

// ---------------------------------------------------------------------------

void Tui::start_turn(const std::string& text) {
    busy_.store(true);
    cancel_.store(false);
    status_text_ = "thinking";

    worker_ = std::thread([this, text]() {
        Agent::Events ev;

        ev.on_text = [this](const std::string& d) {
            queue_.push({Event::Text, d, "", false});
        };
        ev.on_reasoning = [this](const std::string& d) {
            queue_.push({Event::Reasoning, d, "", false});
        };
        ev.on_status = [this](const std::string& s) {
            queue_.push({Event::Status, s, "", false});
        };
        ev.on_error = [this](const std::string& s) {
            queue_.push({Event::Error, s, "", false});
        };
        ev.on_tool_start = [this](const ToolCall& tc) {
            queue_.push({Event::ToolStart, tc.name,
                         json_preview(tc.arguments, 160), false});
        };
        ev.on_tool_done = [this](const ToolCall& tc, const ToolResult& tr) {
            queue_.push({Event::ToolDone, tc.name, tr.content, tr.is_error});
        };

        ev.approve = [this](const std::string& name, ToolKind,
                            const ToolPreview& pv) -> bool {
            if (cfg_.yolo) return true;
            std::unique_lock<std::mutex> lk(gate_.mu);
            gate_.pending = true;
            gate_.answered = false;
            gate_.tool_name = name;
            gate_.preview = pv;
            lk.unlock();

            queue_.push({Event::Approval, name, pv.title + "\n" + pv.detail, false});

            lk.lock();
            gate_.cv.wait(lk, [this] {
                return gate_.answered || cancel_.load() || quit_.load();
            });
            bool allowed = gate_.answered && gate_.allowed;
            gate_.pending = false;
            return allowed;
        };

        Agent::RunResult r = agent_.run(text, ev, &cancel_);

        Event done;
        done.type = Event::Done;
        done.a = r.error;
        done.b = std::to_string(r.rounds);
        done.flag = r.ok;
        queue_.push(done);
    });
}

void Tui::finish_worker() {
    if (worker_.joinable()) worker_.join();
    busy_.store(false);
    approving_ = false;
    status_text_.clear();
}

void Tui::pump_events() {
    Event e;
    int budget = 200;      // don't starve the input loop on a fast stream
    while (budget-- > 0 && queue_.try_pop(&e)) {
        switch (e.type) {
            case Event::Text:
                append_stream(Kind::Assistant, e.a);
                break;
            case Event::Reasoning:
                append_stream(Kind::Reasoning, e.a);
                break;
            case Event::Status:
                status_text_ = e.a;
                break;
            case Event::Error:
                add(Kind::Error, e.a);
                break;
            case Event::ToolStart:
                add(Kind::Tool, e.a + "  " + e.b);
                status_text_ = e.a;
                break;
            case Event::ToolDone: {
                std::string body = e.b;
                // Long tool output is collapsed; the model saw all of it.
                std::vector<std::string> lines = split(body, '\n');
                if (lines.size() > 12) {
                    lines.resize(12);
                    body = join(lines, "\n") + "\n... (" +
                           std::to_string(split(e.b, '\n').size()) + " lines total)";
                }
                add(e.flag ? Kind::Error : Kind::ToolOutput, body);
                break;
            }
            case Event::Approval:
                approving_ = true;
                add(Kind::Tool, "needs approval: " + e.b);
                break;
            case Event::Done: {
                finish_worker();
                session_usage_ = agent_.session_usage();
                if (!e.flag && !e.a.empty() && e.a != "cancelled")
                    add(Kind::Error, e.a);
                else if (e.a == "cancelled")
                    add(Kind::Status, "(cancelled)");
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------

bool Tui::handle_slash(const std::string& line) {
    std::vector<std::string> parts = split(trim(line), ' ');
    std::string cmd = parts.empty() ? "" : parts[0];
    std::string rest = trim(line.size() > cmd.size() ? line.substr(cmd.size()) : "");

    if (cmd == "/help") {
        add(Kind::Info,
            "Commands:\n"
            "  /model [id]     show or change the model\n"
            "  /models [sub]   list models, optionally filtered\n"
            "  /tools          list available tools\n"
            "  /mcp            show connected MCP servers\n"
            "  /cwd [dir]      show or change the working directory\n"
            "  /yolo           toggle approving every tool automatically\n"
            "  /clear          start a fresh conversation\n"
            "  /save PATH      write this session to a file\n"
            "  /load PATH      restore a session\n"
            "  /cost           show token and cost totals\n"
            "  /quit           exit\n"
            "\n"
            "Keys: Enter sends, Ctrl+J newline, Ctrl+C cancels, Ctrl+D quits,\n"
            "      PgUp/PgDn scroll, Up/Down recall history.");
        return true;
    }
    if (cmd == "/quit" || cmd == "/exit") { quit_.store(true); return true; }
    if (cmd == "/clear") {
        agent_.reset();
        entries_.clear();
        dirty_ = true;
        add(Kind::Info, "conversation cleared");
        return true;
    }
    if (cmd == "/model") {
        if (rest.empty()) { add(Kind::Info, "model: " + cfg_.model); return true; }
        cfg_.model = rest;
        client_.set_model(rest);
        add(Kind::Info, "model set to " + rest);
        return true;
    }
    if (cmd == "/models") {
        std::string err;
        std::vector<ModelInfo> ms = client_.list_models(&err);
        if (ms.empty()) { add(Kind::Error, "could not list models: " + err); return true; }
        std::string needle = to_lower(rest);
        std::string out;
        int n = 0;
        for (const ModelInfo& m : ms) {
            if (!needle.empty() && to_lower(m.id).find(needle) == std::string::npos)
                continue;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%-46s $%.2f/$%.2f per Mtok%s",
                          m.id.c_str(), m.prompt_cost * 1e6, m.completion_cost * 1e6,
                          m.supports_tools ? " [tools]" : "");
            out += std::string(buf) + "\n";
            if (++n >= 60) { out += "... (narrow with /models <substring>)\n"; break; }
        }
        add(Kind::Info, out.empty() ? "no matches" : out);
        return true;
    }
    if (cmd == "/tools") {
        std::string out;
        for (const std::string& n : tools_.names()) {
            const Tool* t = tools_.find(n);
            out += "  " + n + (t && t->source != "builtin" ? "  [" + t->source + "]" : "") + "\n";
        }
        add(Kind::Info, "tools:\n" + out);
        return true;
    }
    if (cmd == "/mcp") {
        if (!mcp_ || mcp_->server_count() == 0) {
            add(Kind::Info,
                "no MCP servers connected. Add them to " + cfg_.config_path +
                " under \"mcp_servers\".");
            return true;
        }
        std::string out;
        for (const std::string& l : mcp_->status_lines()) out += "  " + l + "\n";
        add(Kind::Info, "MCP servers:\n" + out);
        return true;
    }
    if (cmd == "/cwd") {
        if (rest.empty()) { add(Kind::Info, "cwd: " + agent_.cwd()); return true; }
        std::string d = expand_user(rest);
        if (chdir(d.c_str()) != 0) { add(Kind::Error, "cannot enter " + d); return true; }
        char buf[4096];
        agent_.set_cwd(getcwd(buf, sizeof(buf)) ? buf : d);
        add(Kind::Info, "cwd: " + agent_.cwd());
        return true;
    }
    if (cmd == "/yolo") {
        cfg_.yolo = !cfg_.yolo;
        add(Kind::Info, cfg_.yolo ? "yolo on -- tools run without asking"
                                  : "yolo off -- mutating tools will ask");
        return true;
    }
    if (cmd == "/cost") {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "prompt %lld, completion %lld, total %lld tokens, $%.4f",
                      static_cast<long long>(session_usage_.prompt_tokens),
                      static_cast<long long>(session_usage_.completion_tokens),
                      static_cast<long long>(session_usage_.total_tokens),
                      session_usage_.cost);
        add(Kind::Info, buf);
        return true;
    }
    if (cmd == "/save") {
        if (rest.empty()) { add(Kind::Error, "usage: /save PATH"); return true; }
        std::string err;
        if (write_file_text(expand_user(rest), agent_.to_json().dump(2) + "\n", &err))
            add(Kind::Info, "saved to " + rest);
        else
            add(Kind::Error, err);
        return true;
    }
    if (cmd == "/load") {
        if (rest.empty()) { add(Kind::Error, "usage: /load PATH"); return true; }
        std::string text, err;
        if (!read_file_text(expand_user(rest), &text, &err)) { add(Kind::Error, err); return true; }
        try {
            if (agent_.from_json(json::parse(text), &err)) {
                entries_.clear();
                dirty_ = true;
                add(Kind::Info, "loaded " + rest + " (" +
                                std::to_string(agent_.history().size()) + " messages)");
            } else {
                add(Kind::Error, err);
            }
        } catch (const std::exception& ex) {
            add(Kind::Error, ex.what());
        }
        return true;
    }
    if (!cmd.empty() && cmd[0] == '/') {
        add(Kind::Error, "unknown command " + cmd + " (try /help)");
        return true;
    }
    return false;
}

void Tui::submit() {
    std::string line = ed_.text;
    if (trim(line).empty()) return;
    ed_.push_history(line);
    ed_.clear();
    ed_.hist_pos = -1;

    if (starts_with(trim(line), "/")) {
        add(Kind::User, line);
        handle_slash(trim(line));
        return;
    }
    add(Kind::User, line);
    start_turn(line);
}

void Tui::handle_key(int ch) {
    // Approval mode swallows keys until answered.
    if (approving_) {
        bool decided = false, allow = false;
        if (ch == 'y' || ch == 'Y') { decided = true; allow = true; }
        else if (ch == 'n' || ch == 'N' || ch == 27) { decided = true; allow = false; }
        else if (ch == 'a' || ch == 'A') {
            cfg_.yolo = true;
            decided = true;
            allow = true;
            add(Kind::Info, "approving all tools for the rest of this session");
        } else if (ch == 3) {                 // Ctrl+C
            cancel_.store(true);
            decided = true;
            allow = false;
        }
        if (decided) {
            {
                std::lock_guard<std::mutex> lk(gate_.mu);
                gate_.answered = true;
                gate_.allowed = allow;
            }
            gate_.cv.notify_all();
            approving_ = false;
            add(Kind::Status, allow ? "approved" : "denied");
        }
        return;
    }

    switch (ch) {
        case 3:                                // Ctrl+C
            if (busy_.load()) {
                cancel_.store(true);
                status_text_ = "cancelling";
            } else if (!ed_.text.empty()) {
                ed_.clear();
            } else {
                add(Kind::Info, "Ctrl+D or /quit to exit");
            }
            return;
        case 4:                                // Ctrl+D
            if (ed_.text.empty()) quit_.store(true);
            else ed_.del();
            return;
        // Return arrives as '\r' (13) or KEY_ENTER in raw mode. A bare 0x0A
        // (Ctrl+J) is intercepted in run() and inserts a newline instead.
        case '\r':
        case KEY_ENTER:
            if (!busy_.load()) submit();
            return;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            ed_.backspace();
            return;
        case KEY_DC:  ed_.del();   return;
        case KEY_LEFT:  ed_.left();  return;
        case KEY_RIGHT: ed_.right(); return;
        case KEY_HOME: case 1: ed_.home(); return;   // Ctrl+A
        case KEY_END:  case 5: ed_.end();  return;   // Ctrl+E
        case 11: ed_.kill_to_end(); return;          // Ctrl+K
        case 21: ed_.clear(); return;                // Ctrl+U
        case KEY_UP:
            if (ed_.text.find('\n') == std::string::npos) ed_.hist_prev();
            return;
        case KEY_DOWN:
            if (ed_.text.find('\n') == std::string::npos) ed_.hist_next();
            return;
        case KEY_PPAGE: {
            int h, w; getmaxyx(stdscr, h, w); (void)w;
            scroll_ += std::max(1, h / 2);
            follow_ = false;
            return;
        }
        case KEY_NPAGE: {
            int h, w; getmaxyx(stdscr, h, w); (void)w;
            scroll_ -= std::max(1, h / 2);
            if (scroll_ <= 0) { scroll_ = 0; follow_ = true; }
            return;
        }
        case KEY_RESIZE:
            dirty_ = true;
            return;
        default:
            break;
    }

    if (ch == 12) { dirty_ = true; clearok(stdscr, TRUE); return; }   // Ctrl+L

    if (ch >= 32 && ch < 127) {
        ed_.insert(static_cast<char>(ch));
        ed_.hist_pos = -1;
    } else if (ch == 9) {                     // Tab -> two spaces
        ed_.insert_str("  ");
    }
}

int Tui::run() {
    initscr();
    raw();                 // deliver Ctrl+C as a key rather than a signal
    nonl();                // keep Return (13) distinct from Ctrl+J (10);
                           // with nl() on, ncurses folds CR into LF and there
                           // is no way to tell "send" from "insert newline"
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);
    scrollok(stdscr, FALSE);

    color_ = cfg_.color && has_colors();
    if (color_) {
        start_color();
        use_default_colors();
        init_pair(CP_USER,   COLOR_CYAN,    -1);
        init_pair(CP_ASSIST, -1,            -1);
        init_pair(CP_TOOL,   COLOR_YELLOW,  -1);
        init_pair(CP_ERROR,  COLOR_RED,     -1);
        init_pair(CP_STATUS, COLOR_GREEN,   -1);
        init_pair(CP_DIM,    COLOR_BLUE,    -1);
        init_pair(CP_PROMPT, COLOR_MAGENTA, -1);
        init_pair(CP_BAR,    COLOR_BLACK,   COLOR_CYAN);
    }

    add(Kind::Info,
        "ppcode -- PowerPC Leopard build. /help for commands, Ctrl+D to quit.\n"
        "model: " + cfg_.model + "    cwd: " + agent_.cwd() + "    tools: " +
        std::to_string(tools_.size()));

    int tick = 0;
    while (!quit_.load()) {
        pump_events();

        int ch = getch();
        while (ch != ERR) {
            // Ctrl+J arrives as 0x0A which collides with Enter on some
            // terminals; ncurses gives KEY_ENTER/'\r' for the real Return key
            // in raw mode, so treat a bare 0x0A as "insert newline".
            if (ch == 10 && !busy_.load()) {
                ed_.insert('\n');
            } else {
                handle_key(ch);
            }
            if (quit_.load()) break;
            ch = getch();
        }

        if (++tick % 2 == 0) spinner_++;
        draw();
        napms(50);
    }

    // Unblock the worker if it is parked on the approval gate.
    cancel_.store(true);
    {
        std::lock_guard<std::mutex> lk(gate_.mu);
        gate_.answered = true;
        gate_.allowed = false;
    }
    gate_.cv.notify_all();
    if (worker_.joinable()) worker_.join();

    endwin();

    if (session_usage_.total_tokens > 0)
        std::printf("%lld tokens, $%.4f this session\n",
                    static_cast<long long>(session_usage_.total_tokens),
                    session_usage_.cost);
    return 0;
}

} // namespace

int run_tui(Agent& agent, Client& client, ToolRegistry& tools, const Config& cfg,
            mcp::Manager* mcp_manager) {
    Tui tui(agent, client, tools, cfg, mcp_manager);
    return tui.run();
}

} // namespace ppcode

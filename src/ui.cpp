// ui.cpp -- ncurses front end.
//
// Threading: ncurses is not thread-safe, so only the main thread touches it.
// The agent runs on a worker thread and communicates by pushing events onto a
// queue. Tool approval reverses the direction -- the worker blocks on a
// condition variable while the main thread collects the answer.
//
// Rendering goes through render::Line (styled spans) rather than raw strings, so
// markdown and syntax highlighting come out of the same path as plain text, and
// all measurement is in display columns rather than bytes.
#include "ui.hpp"

#include "jobs.hpp"
#include "session.hpp"
#include "render.hpp"
#include "utf8.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <langinfo.h>
#include <locale.h>
#include <ncurses.h>
#include <unistd.h>

namespace ppcode {
namespace {

// ---------------------------------------------------------------------------
// Locale
// ---------------------------------------------------------------------------

// LC_CTYPE is "C" on a stock Leopard login, which makes ncurses treat every byte
// as its own character and paints multi-byte text as garbage. Try to get into a
// UTF-8 locale before initscr(); report what we ended up with so the caller can
// decide whether to use box-drawing characters.
bool setup_locale() {
    setlocale(LC_ALL, "");
    auto is_utf8 = []() {
        const char* cs = nl_langinfo(CODESET);
        if (!cs) return false;
        std::string s = to_lower(cs);
        return s == "utf-8" || s == "utf8";
    };
    if (is_utf8()) return true;
    for (const char* cand : {"en_US.UTF-8", "UTF-8", "C.UTF-8"}) {
        if (setlocale(LC_ALL, cand) && is_utf8()) return true;
    }
    setlocale(LC_ALL, "");
    return false;
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

class Palette {
public:
    void init(bool want_color) {
        has_color_ = want_color && has_colors();
        if (!has_color_) {
            depth_ = render::ColorDepth::Mono;
            return;
        }
        start_color();
        use_default_colors();
        depth_ = render::detect_depth(COLORS, true);

        // With a redefinable palette we can set our exact RGB values, which
        // gives full fidelity even though only ~28 slots are needed.
        custom_ = can_change_color() && COLORS >= 256 && COLOR_PAIRS > kCount + 2;

        for (int i = 0; i < kCount; i++) {
            const render::StyleDef& d =
                render::style_def(static_cast<render::Style>(i));
            short fg;
            if (custom_) {
                short slot = static_cast<short>(kFirstSlot + i);
                init_color(slot,
                           static_cast<short>(d.fg.r * 1000 / 255),
                           static_cast<short>(d.fg.g * 1000 / 255),
                           static_cast<short>(d.fg.b * 1000 / 255));
                fg = slot;
            } else if (depth_ == render::ColorDepth::Ansi16) {
                fg = static_cast<short>(d.ansi16 >= 0 ? d.ansi16
                                                      : render::rgb_to_16(d.fg));
            } else {
                fg = static_cast<short>(render::rgb_to_256(d.fg));
            }
            init_pair(static_cast<short>(i + 1), fg, -1);
        }
        // The status bar is the one place we want a filled background.
        init_pair(kBarPair, COLOR_BLACK, COLOR_CYAN);
    }

    int attr(render::Style s) const {
        const render::StyleDef& d = render::style_def(s);
        int a = 0;
        if (d.bold) a |= A_BOLD;
        if (d.underline) a |= A_UNDERLINE;
        if (d.dim) a |= A_DIM;
        if (d.reverse) a |= A_REVERSE;
        if (!has_color_) return a;
        if (s == render::Style::Bar) return COLOR_PAIR(kBarPair) | (a & ~A_REVERSE);
        return a | COLOR_PAIR(static_cast<int>(s) + 1);
    }

    render::ColorDepth depth() const { return depth_; }
    bool custom() const { return custom_; }

private:
    static constexpr int kCount = static_cast<int>(render::Style::Count_);
    static constexpr int kFirstSlot = 32;      // leave the standard 16 alone
    static constexpr short kBarPair = kCount + 1;

    bool has_color_ = false;
    bool custom_ = false;
    render::ColorDepth depth_ = render::ColorDepth::Mono;
};

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

enum class Kind { User, Assistant, Tool, ToolOutput, Status, Error, Info, Reasoning };

struct Entry {
    Kind kind;
    std::string text;
    // Assistant text is rendered as markdown; everything else stays verbatim so
    // that tool output and diagnostics are never reinterpreted.
    bool markdown = false;
};

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
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back(std::move(e));
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
    std::deque<Event> q_;
};

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
// Input editor -- all positions are byte offsets on codepoint boundaries.
// ---------------------------------------------------------------------------

struct Editor {
    std::string text;
    size_t cursor = 0;
    std::vector<std::string> history;
    int hist_pos = -1;
    std::string stash;

    void insert(const std::string& s) {
        text.insert(cursor, s);
        cursor += s.size();
    }
    void backspace() {
        if (cursor == 0) return;
        size_t prev = utf8::step(text, cursor, -1);
        text.erase(prev, cursor - prev);
        cursor = prev;
    }
    void del() {
        if (cursor >= text.size()) return;
        size_t next = utf8::step(text, cursor, 1);
        text.erase(cursor, next - cursor);
    }
    void left()  { cursor = utf8::step(text, cursor, -1); }
    void right() { cursor = utf8::step(text, cursor, 1); }

    size_t line_start(size_t pos) const {
        if (pos == 0) return 0;
        size_t p = text.rfind('\n', pos - 1);
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
    void kill_word() {
        // Back over spaces, then over the word.
        size_t p = cursor;
        while (p > 0) {
            size_t prev = utf8::step(text, p, -1);
            if (text[prev] != ' ' && text[prev] != '\t') break;
            p = prev;
        }
        while (p > 0) {
            size_t prev = utf8::step(text, p, -1);
            if (text[prev] == ' ' || text[prev] == '\t' || text[prev] == '\n') break;
            p = prev;
        }
        text.erase(p, cursor - p);
        cursor = p;
    }
    void clear() { text.clear(); cursor = 0; }

    void push_history(const std::string& s) {
        if (s.empty()) return;
        if (!history.empty() && history.back() == s) return;
        history.push_back(s);
        if (history.size() > 300) history.erase(history.begin());
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
// Model picker overlay
// ---------------------------------------------------------------------------

struct Picker {
    bool active = false;
    std::string query;
    std::vector<const ModelInfo*> results;
    size_t selected = 0;
    size_t scroll = 0;
    std::string status;
};

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
    Palette pal_;
    Picker picker_;
    ModelCatalog catalog_;
    JobManager jobs_;

    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> quit_{false};

    Editor ed_;
    int scroll_ = 0;             // lines above the live tail
    bool follow_ = true;
    std::string status_text_;
    int spinner_ = 0;
    Usage session_usage_;
    bool utf8_ok_ = false;
    bool approving_ = false;
    bool needs_redraw_ = true;
    int steering_queued_ = 0;

    // Cached wrap of the transcript.
    std::vector<render::Line> wrapped_;
    bool dirty_ = true;
    int wrapped_width_ = -1;

    void add(Kind k, const std::string& text, bool markdown = false);
    void append_stream(Kind k, const std::string& delta);
    void rewrap(int width);
    void draw();
    void draw_transcript(int top, int height, int width);
    void draw_status(int row, int width);
    void draw_input(int top, int height, int width);
    void draw_picker(int width, int height);
    int input_rows(int width) const;

    void handle_key(int ch);
    bool handle_mouse();
    void handle_picker_key(int ch);
    void open_picker(const std::string& initial);
    void refresh_picker();
    void submit();
    bool handle_slash(const std::string& line);
    void start_turn(const std::string& text);
    void pump_events();
    void finish_worker();
    void scroll_by(int lines);

    std::string glyph(const char* uni, const char* ascii) const {
        return (cfg_.unicode && utf8_ok_) ? uni : ascii;
    }
    bool fancy() const { return cfg_.unicode && utf8_ok_; }
};

// ---------------------------------------------------------------------------

void Tui::add(Kind k, const std::string& text, bool markdown) {
    entries_.push_back({k, text, markdown});
    dirty_ = true;
    needs_redraw_ = true;
    if (follow_) scroll_ = 0;
}

void Tui::append_stream(Kind k, const std::string& delta) {
    if (!entries_.empty() && entries_.back().kind == k) {
        entries_.back().text += delta;
    } else {
        entries_.push_back({k, delta, k == Kind::Assistant});
    }
    dirty_ = true;
    needs_redraw_ = true;
    if (follow_) scroll_ = 0;
}

void Tui::rewrap(int width) {
    if (!dirty_ && wrapped_width_ == width) return;
    wrapped_.clear();
    wrapped_width_ = width;

    for (const Entry& e : entries_) {
        std::string prefix;
        render::Style base = render::Style::Plain;
        switch (e.kind) {
            case Kind::User:       prefix = glyph("\xE2\x80\xBA ", "> ");
                                   base = render::Style::UserText; break;
            case Kind::Tool:       prefix = glyph("\xE2\x97\x8F ", "* ");
                                   base = render::Style::ToolName; break;
            case Kind::ToolOutput: prefix = "    ";
                                   base = render::Style::ToolOutput; break;
            case Kind::Error:      prefix = "! ";
                                   base = render::Style::ErrorText; break;
            case Kind::Status:     prefix = "  ";
                                   base = render::Style::StatusText; break;
            case Kind::Info:       prefix = "  ";
                                   base = render::Style::StatusText; break;
            case Kind::Reasoning:  prefix = "  ";
                                   base = render::Style::Dim; break;
            case Kind::Assistant:  prefix = "";
                                   base = render::Style::Plain; break;
        }
        int avail = width - static_cast<int>(utf8::width(prefix));
        if (avail < 12) avail = 12;

        std::vector<render::Line> lines;
        if (e.markdown) {
            lines = render::markdown(e.text, static_cast<size_t>(avail), fancy());
        } else {
            lines = render::plain_lines(e.text, static_cast<size_t>(avail), base);
        }

        std::string cont(utf8::width(prefix), ' ');
        for (size_t i = 0; i < lines.size(); i++) {
            render::Line& l = lines[i];
            // Fold the entry prefix into the line's gutter, keeping any gutter
            // the markdown renderer already assigned (code borders, quotes).
            l.gutter = (i == 0 ? prefix : cont) + l.gutter;
            if (l.gutter_style == render::Style::Dim && i == 0)
                l.gutter_style = base;
            wrapped_.push_back(std::move(l));
        }
        wrapped_.push_back(render::Line());
    }
    dirty_ = false;
}

int Tui::input_rows(int width) const {
    int avail = width - 2;
    if (avail < 8) avail = 8;
    size_t n = utf8::wrap(ed_.text.empty() ? " " : ed_.text,
                          static_cast<size_t>(avail)).size();
    return std::max<int>(1, std::min<int>(static_cast<int>(n), 10));
}

void Tui::scroll_by(int lines) {
    scroll_ += lines;
    if (scroll_ <= 0) { scroll_ = 0; follow_ = true; }
    else follow_ = false;
    needs_redraw_ = true;
}

void Tui::draw_transcript(int top, int height, int width) {
    rewrap(width);
    int total = static_cast<int>(wrapped_.size());
    int max_scroll = std::max(0, total - height);
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    if (scroll_ < 0) scroll_ = 0;

    int start = std::max(0, total - height - scroll_);
    for (int r = 0; r < height; r++) {
        move(top + r, 0);
        clrtoeol();
        int idx = start + r;
        if (idx < 0 || idx >= total) continue;

        const render::Line& line = wrapped_[static_cast<size_t>(idx)];
        int col = 0;
        if (!line.gutter.empty()) {
            int a = pal_.attr(line.gutter_style);
            attron(a);
            mvaddstr(top + r, col, line.gutter.c_str());
            attroff(a);
            col += static_cast<int>(utf8::width(line.gutter));
        }
        for (const render::Span& sp : line.spans) {
            if (col >= width) break;
            std::string piece = utf8::truncate_to_width(
                sp.text, static_cast<size_t>(width - col));
            if (piece.empty()) break;
            int a = pal_.attr(sp.style);
            attron(a);
            mvaddstr(top + r, col, piece.c_str());
            attroff(a);
            col += static_cast<int>(utf8::width(piece));
        }
    }
}

void Tui::draw_status(int row, int width) {
    std::string left;
    if (approving_) {
        left = "APPROVE?  type y / n / a then Enter   (Ctrl+Y yes, Ctrl+N no)";
    } else if (busy_.load()) {
        static const char* frames = "|/-\\";
        left = std::string(1, frames[spinner_ % 4]) + " " +
               (status_text_.empty() ? "working" : status_text_);
        if (steering_queued_ > 0)
            left += "  [" + std::to_string(steering_queued_) + " queued]";
        left += "  (type to steer, Ctrl+C cancels)";
    } else if (!follow_) {
        left = "scrolled up " + std::to_string(scroll_) +
               " lines -- End or PgDn to return";
    } else {
        left = status_text_.empty() ? "ready" : status_text_;
    }

    char right[256];
    std::snprintf(right, sizeof(right), "%s  %lldtok  $%.4f",
                  utf8::elide(cfg_.model, 30).c_str(),
                  static_cast<long long>(session_usage_.total_tokens),
                  session_usage_.cost);

    size_t rlen = utf8::width(right);
    std::string bar = left;
    if (utf8::width(bar) + rlen + 2 > static_cast<size_t>(width))
        bar = utf8::elide(bar, width > static_cast<int>(rlen) + 3
                                   ? width - rlen - 3 : 1);
    size_t pad = static_cast<size_t>(width) - utf8::width(bar) - rlen;
    bar += std::string(pad, ' ');
    bar += right;

    int a = pal_.attr(render::Style::Bar);
    attron(a);
    move(row, 0);
    clrtoeol();
    mvaddstr(row, 0, utf8::truncate_to_width(bar, static_cast<size_t>(width)).c_str());
    attroff(a);
}

void Tui::draw_input(int top, int height, int width) {
    std::string prompt = glyph("\xE2\x9D\xAF ", "> ");
    size_t pw = utf8::width(prompt);
    int avail = width - static_cast<int>(pw);
    if (avail < 8) avail = 8;

    // Wrap while tracking which wrapped row and column the cursor lands on.
    std::vector<std::string> lines;
    int cur_row = 0, cur_col = 0;
    {
        size_t consumed = 0;
        for (const std::string& para : split(ed_.text, '\n')) {
            std::vector<std::string> wl = utf8::wrap(para, static_cast<size_t>(avail));
            if (wl.empty()) wl.push_back("");
            for (const std::string& piece : wl) {
                // Is the cursor inside this piece?
                if (ed_.cursor >= consumed && ed_.cursor <= consumed + piece.size()) {
                    cur_row = static_cast<int>(lines.size());
                    cur_col = static_cast<int>(
                        utf8::width(piece.substr(0, ed_.cursor - consumed)));
                }
                lines.push_back(piece);
                // +1 for the break consumed by wrapping or the newline.
                consumed += piece.size() + 1;
            }
        }
        if (lines.empty()) lines.push_back("");
    }

    int first = std::max(0, cur_row - height + 1);
    for (int r = 0; r < height; r++) {
        move(top + r, 0);
        clrtoeol();
        size_t idx = static_cast<size_t>(first + r);
        if (idx >= lines.size()) continue;

        int a = pal_.attr(render::Style::Prompt);
        attron(a);
        mvaddstr(top + r, 0, idx == 0 ? prompt.c_str()
                                      : std::string(pw, ' ').c_str());
        attroff(a);
        mvaddstr(top + r, static_cast<int>(pw),
                 utf8::truncate_to_width(lines[idx],
                                         static_cast<size_t>(avail)).c_str());
    }

    int scr_row = top + (cur_row - first);
    if (scr_row >= top && scr_row < top + height)
        move(scr_row, std::min<int>(static_cast<int>(pw) + cur_col, width - 1));
}

void Tui::draw_picker(int width, int height) {
    // A centred panel over the transcript.
    int w = std::min(width - 4, 96);
    int h = std::min(height - 4, 22);
    if (w < 30 || h < 8) return;
    int x = (width - w) / 2;
    int y = (height - h) / 2;

    // Note: do not name a local "hline", "vline", "border" or similar here --
    // ncurses defines those as macros that inject stdscr as a first argument.
    int frame = pal_.attr(render::Style::Heading1);

    for (int r = 0; r < h; r++) {
        move(y + r, x);
        attron(frame);
        for (int c = 0; c < w; c++) addch(' ');
        attroff(frame);
    }

    attron(frame);
    std::string title = " Select a model ";
    mvaddstr(y, x + 1, title.c_str());
    attroff(frame);

    int a_dim = pal_.attr(render::Style::Dim);
    attron(a_dim);
    mvaddstr(y + 1, x + 1,
             utf8::truncate_to_width("type to filter, Up/Down to move, Enter to "
                                     "choose, Esc to cancel",
                                     static_cast<size_t>(w - 2)).c_str());
    attroff(a_dim);

    int a_prompt = pal_.attr(render::Style::Prompt);
    attron(a_prompt);
    mvaddstr(y + 2, x + 1, "/ ");
    attroff(a_prompt);
    mvaddstr(y + 2, x + 3,
             utf8::truncate_to_width(picker_.query,
                                     static_cast<size_t>(w - 5)).c_str());

    int list_top = y + 4;
    int list_h = h - 5;
    if (picker_.results.empty()) {
        attron(a_dim);
        mvaddstr(list_top, x + 1,
                 picker_.status.empty() ? "no matches" : picker_.status.c_str());
        attroff(a_dim);
    }

    if (picker_.selected < picker_.scroll) picker_.scroll = picker_.selected;
    if (picker_.selected >= picker_.scroll + static_cast<size_t>(list_h))
        picker_.scroll = picker_.selected - static_cast<size_t>(list_h) + 1;

    for (int r = 0; r < list_h; r++) {
        size_t idx = picker_.scroll + static_cast<size_t>(r);
        if (idx >= picker_.results.size()) break;
        const ModelInfo* m = picker_.results[idx];
        bool sel = (idx == picker_.selected);

        // id, context, price, and capability flags -- enough to choose without
        // going and looking anything up.
        char ctx[32];
        if (m->context_length >= 1000000)
            std::snprintf(ctx, sizeof(ctx), "%lldM",
                          static_cast<long long>(m->context_length / 1000000));
        else if (m->context_length >= 1000)
            std::snprintf(ctx, sizeof(ctx), "%lldK",
                          static_cast<long long>(m->context_length / 1000));
        else
            std::snprintf(ctx, sizeof(ctx), "%lld",
                          static_cast<long long>(m->context_length));

        // OpenRouter reports a negative price for routing pseudo-models such as
        // openrouter/auto, where the real cost depends on what it picks.
        char price[48];
        if (m->prompt_cost < 0 || m->completion_cost < 0)
            std::snprintf(price, sizeof(price), "%15s", "varies");
        else
            std::snprintf(price, sizeof(price), "$%6.2f/$%6.2f",
                          m->prompt_cost * 1e6, m->completion_cost * 1e6);

        char row[512];
        std::snprintf(row, sizeof(row), "%-44s %6s  %s %s%s",
                      utf8::elide(m->id, 44).c_str(), ctx, price,
                      m->supports_tools ? "T" : " ",
                      m->supports_images ? "V" : " ");

        int at = sel ? (pal_.attr(render::Style::Bar))
                     : pal_.attr(render::Style::Plain);
        attron(at);
        const size_t field = static_cast<size_t>(w - 2);
        std::string text = std::string(sel ? "> " : "  ") + row;
        std::string padded = utf8::truncate_to_width(text, field);
        // Guard the subtraction: an underflow here would build a gigabyte-long
        // string and wrap the row across the whole screen.
        size_t have = utf8::width(padded);
        if (have < field) padded += std::string(field - have, ' ');
        mvaddstr(list_top + r, x + 1, padded.c_str());
        attroff(at);
    }

    // Footer: count and legend.
    char foot[128];
    std::snprintf(foot, sizeof(foot), "%zu match%s   T=tools V=vision   $ per Mtok",
                  picker_.results.size(),
                  picker_.results.size() == 1 ? "" : "es");
    attron(a_dim);
    mvaddstr(y + h - 1, x + 1,
             utf8::truncate_to_width(foot, static_cast<size_t>(w - 2)).c_str());
    attroff(a_dim);
}

void Tui::draw() {
    int h, w;
    getmaxyx(stdscr, h, w);
    if (h < 8 || w < 30) {
        erase();
        mvaddstr(0, 0, "terminal too small");
        refresh();
        return;
    }

    int in_rows = input_rows(w);
    if (in_rows > h - 3) in_rows = std::max(1, h - 3);
    int trans_h = h - in_rows - 1;
    if (trans_h < 1) trans_h = 1;

    draw_transcript(0, trans_h, w);
    draw_status(trans_h, w);
    draw_input(trans_h + 1, in_rows, w);

    if (picker_.active) {
        draw_picker(w, h);
        curs_set(0);
    } else {
        curs_set(1);
    }
    refresh();
    needs_redraw_ = false;
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
            queue_.push({Event::ToolStart, tc.name, json_preview(tc.arguments, 160),
                         false});
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
    steering_queued_ = 0;
}

void Tui::pump_events() {
    Event e;
    int budget = 400;
    while (budget-- > 0 && queue_.try_pop(&e)) {
        switch (e.type) {
            case Event::Text:      append_stream(Kind::Assistant, e.a); break;
            case Event::Reasoning: append_stream(Kind::Reasoning, e.a); break;
            case Event::Status:    status_text_ = e.a; needs_redraw_ = true; break;
            case Event::Error:     add(Kind::Error, e.a); break;
            case Event::ToolStart:
                add(Kind::Tool, e.a + "  " + e.b);
                status_text_ = e.a;
                break;
            case Event::ToolDone: {
                std::string body = e.b;
                std::vector<std::string> lines = split(body, '\n');
                if (lines.size() > 14) {
                    size_t total = lines.size();
                    lines.resize(14);
                    body = join(lines, "\n") + "\n... (" + std::to_string(total) +
                           " lines total)";
                }
                add(e.flag ? Kind::Error : Kind::ToolOutput, body);
                break;
            }
            case Event::Approval:
                approving_ = true;
                add(Kind::Tool, "needs approval: " + e.b);
                break;
            case Event::Done:
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

// ---------------------------------------------------------------------------
// Model picker
// ---------------------------------------------------------------------------

void Tui::refresh_picker() {
    picker_.results.clear();
    picker_.selected = 0;
    picker_.scroll = 0;

    if (catalog_.empty()) {
        picker_.status = "model list unavailable";
        return;
    }

    if (trim(picker_.query).empty()) {
        // With no query, lead with the favourites so the common choices are one
        // keystroke away rather than buried in 300+ entries.
        for (const std::string& id : favorite_models())
            if (const ModelInfo* m = catalog_.find(id)) picker_.results.push_back(m);
        size_t favs = picker_.results.size();
        for (const ModelInfo* m : catalog_.search("", 400)) {
            bool dup = false;
            for (size_t i = 0; i < favs; i++)
                if (picker_.results[i]->id == m->id) dup = true;
            if (!dup) picker_.results.push_back(m);
        }
        picker_.status = "favourites first";
        return;
    }
    picker_.results = catalog_.search(picker_.query, 400);
    picker_.status.clear();
}

void Tui::open_picker(const std::string& initial) {
    if (catalog_.empty()) {
        add(Kind::Status, "fetching model list...");
        draw();
        std::string err;
        if (!catalog_.load(client_, &err)) {
            add(Kind::Error, "could not load models: " + err);
            return;
        }
    }
    picker_.active = true;
    picker_.query = initial;
    refresh_picker();
    needs_redraw_ = true;
}

void Tui::handle_picker_key(int ch) {
    switch (ch) {
        case 27:                       // Esc
            picker_.active = false;
            needs_redraw_ = true;
            return;
        case '\r':
        case KEY_ENTER:
            if (!picker_.results.empty()) {
                const ModelInfo* m = picker_.results[picker_.selected];
                cfg_.model = m->id;
                client_.set_model(m->id);
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "model set to %s (context %lld, $%.2f/$%.2f per Mtok%s%s)",
                              m->id.c_str(),
                              static_cast<long long>(m->context_length),
                              m->prompt_cost * 1e6, m->completion_cost * 1e6,
                              m->supports_tools ? ", tools" : ", NO TOOL SUPPORT",
                              m->supports_images ? ", vision" : "");
                add(Kind::Info, buf);
                if (!m->supports_tools)
                    add(Kind::Error,
                        "This model does not support tool calling, so file and "
                        "shell tools will not work with it.");
            }
            picker_.active = false;
            needs_redraw_ = true;
            return;
        case KEY_UP:
            if (picker_.selected > 0) picker_.selected--;
            needs_redraw_ = true;
            return;
        case KEY_DOWN:
            if (picker_.selected + 1 < picker_.results.size()) picker_.selected++;
            needs_redraw_ = true;
            return;
        case KEY_PPAGE:
            picker_.selected = picker_.selected > 10 ? picker_.selected - 10 : 0;
            needs_redraw_ = true;
            return;
        case KEY_NPAGE:
            picker_.selected = std::min(picker_.results.size() ? picker_.results.size() - 1 : 0,
                                       picker_.selected + 10);
            needs_redraw_ = true;
            return;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            if (!picker_.query.empty()) {
                size_t prev = utf8::step(picker_.query, picker_.query.size(), -1);
                picker_.query.erase(prev);
                refresh_picker();
            }
            needs_redraw_ = true;
            return;
        case 21:                       // Ctrl+U
            picker_.query.clear();
            refresh_picker();
            needs_redraw_ = true;
            return;
        default:
            break;
    }
    if (ch >= 32 && ch < 127) {
        picker_.query += static_cast<char>(ch);
        refresh_picker();
        needs_redraw_ = true;
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
            "  /model [id]     open the model picker, or set a model directly\n"
            "  /models [sub]   list models matching a substring\n"
            "  /tools          list available tools\n"
            "  /mcp            show connected MCP servers\n"
            "  /env [level]    show machine context, or set the detail level\n"
            "  /jobs           list background jobs\n"
            "  /todo           show the current plan\n"
            "  /cwd [dir]      show or change the working directory\n"
            "  /yolo           toggle approving every tool automatically\n"
            "  /unicode        toggle box-drawing and typographic characters\n"
            "  /clear          start a fresh conversation\n"
            "  /compact        summarise the conversation to free up context\n"
            "  /sessions       list saved sessions\n"
            "  /save PATH      write this session to a file\n"
            "  /load PATH      restore a session\n"
            "  /cost           show token and cost totals\n"
            "  /quit           exit\n"
            "\n"
            "Keys:\n"
            "  Enter send. While the model is working, Enter queues a steering\n"
            "    message that is injected after the current step.\n"
            "  Ctrl+J newline, Ctrl+C cancel, Ctrl+D quit\n"
            "  At an approval prompt: type y, n or a then Enter (Ctrl+Y / Ctrl+N)\n"
            "  PgUp/PgDn scroll, Shift+Up/Down scroll a line, Home/End of input\n"
            "  Ctrl+Home top of transcript, Ctrl+End live tail\n"
            "  Mouse wheel scrolls the transcript\n"
            "  Up/Down recall previous inputs, Ctrl+W delete a word");
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
        if (rest.empty()) { open_picker(""); return true; }
        // An exact id is applied directly; anything else opens the picker
        // pre-filtered, so a half-remembered name still works.
        if (catalog_.empty()) catalog_.load(client_, nullptr);
        if (const ModelInfo* m = catalog_.find(rest)) {
            cfg_.model = m->id;
            client_.set_model(m->id);
            add(Kind::Info, "model set to " + m->id);
        } else {
            open_picker(rest);
        }
        return true;
    }
    if (cmd == "/models") {
        if (catalog_.empty()) {
            std::string err;
            if (!catalog_.load(client_, &err)) {
                add(Kind::Error, "could not list models: " + err);
                return true;
            }
        }
        std::string out;
        int n = 0;
        for (const ModelInfo* m : catalog_.search(rest, 80)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%-46s %8lld  $%.2f/$%.2f%s%s",
                          m->id.c_str(), static_cast<long long>(m->context_length),
                          m->prompt_cost * 1e6, m->completion_cost * 1e6,
                          m->supports_tools ? "  tools" : "",
                          m->supports_images ? "  vision" : "");
            out += std::string(buf) + "\n";
            if (++n >= 60) { out += "... (narrow the filter)\n"; break; }
        }
        add(Kind::Info, out.empty() ? "no matches" : out);
        return true;
    }
    if (cmd == "/tools") {
        std::string out;
        for (const std::string& n : tools_.names()) {
            const Tool* t = tools_.find(n);
            out += "  " + n +
                   (t && t->source != "builtin" ? "  [" + t->source + "]" : "") + "\n";
        }
        add(Kind::Info, "tools:\n" + out);
        return true;
    }
    if (cmd == "/env") {
        if (!rest.empty()) {
            bool ok = false;
            envinfo::Detail d = envinfo::detail_from_string(rest, &ok);
            if (!ok) {
                add(Kind::Error,
                    "usage: /env [none|minimal|brief|standard|full]");
                return true;
            }
            envinfo::Probe p = envinfo::probe(false);
            add(Kind::Info, "machine context at " + envinfo::detail_to_string(d) +
                                ":\n" + envinfo::render(p, d));
            return true;
        }
        envinfo::Probe p = envinfo::probe(false);
        add(Kind::Info, envinfo::render(p, envinfo::Detail::Standard));
        return true;
    }
    if (cmd == "/jobs") {
        std::vector<Job> all = jobs_.list();
        if (all.empty()) { add(Kind::Info, "no background jobs"); return true; }
        std::string out;
        for (const Job& j : all) {
            out += "job " + std::to_string(j.id) + "  " +
                   (j.running ? "RUNNING " + j.elapsed()
                              : "exit " + std::to_string(j.exit_code) + " after " +
                                    j.elapsed()) +
                   "\n    " + elide(j.command, 100) + "\n";
        }
        add(Kind::Info, "background jobs:\n" + out);
        return true;
    }
    if (cmd == "/todo") {
        add(Kind::Info, "the plan is shown by the model via todo_write; "
                        "ask it to make one if there is none");
        return true;
    }
    if (cmd == "/mcp") {
        if (!mcp_ || mcp_->server_count() == 0) {
            add(Kind::Info, "no MCP servers connected. Add them to " +
                                cfg_.config_path + " under \"mcp_servers\".");
            return true;
        }
        std::string out;
        for (const std::string& l : mcp_->status_lines()) out += "  " + l + "\n";
        add(Kind::Info, "MCP servers:\n" + out);
        return true;
    }
    if (cmd == "/compact") {
        if (busy_.load()) { add(Kind::Error, "cannot compact while a turn is running"); return true; }
        add(Kind::Status, "summarising the conversation...");
        draw();
        std::string summary, err;
        if (agent_.compact_now(&summary, &err)) {
            entries_.clear();
            dirty_ = true;
            add(Kind::Info, "Conversation compacted. Summary of the earlier work:\n\n" +
                                summary);
        } else {
            add(Kind::Error, "could not compact: " + err);
        }
        return true;
    }
    if (cmd == "/sessions") {
        std::vector<session::Meta> all = session::list(20);
        if (all.empty()) { add(Kind::Info, "no saved sessions"); return true; }
        std::string out;
        for (const session::Meta& m : all)
            out += "  " + m.id + "  " + m.age() + "  " +
                   std::to_string(m.message_count) + " msg\n      " +
                   elide(m.title, 74) + "\n";
        add(Kind::Info, "saved sessions (resume with: ppcode --resume ID):\n" + out);
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
    if (cmd == "/unicode") {
        cfg_.unicode = !cfg_.unicode;
        dirty_ = true;
        add(Kind::Info, std::string("unicode ") + (cfg_.unicode ? "on" : "off") +
                            (utf8_ok_ ? "" : " (terminal locale is not UTF-8, so "
                                             "this may not render)"));
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
        if (!read_file_text(expand_user(rest), &text, &err)) {
            add(Kind::Error, err);
            return true;
        }
        try {
            if (agent_.from_json(json::parse(text), &err)) {
                entries_.clear();
                dirty_ = true;
                add(Kind::Info, "loaded " + rest + " (" +
                                    std::to_string(agent_.history().size()) +
                                    " messages)");
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
    follow_ = true;
    scroll_ = 0;

    // Typing while the model is working steers it rather than being ignored:
    // the text is queued and injected between rounds, so a wrong turn can be
    // corrected without cancelling and throwing away the work so far.
    if (busy_.load()) {
        std::string t = trim(line);
        if (starts_with(t, "/")) {
            add(Kind::Error,
                "slash commands cannot run while a turn is in progress; "
                "press Ctrl+C first");
            return;
        }
        agent_.queue_steering(line);
        add(Kind::User, line);
        add(Kind::Status, "queued -- will be sent to the model after this step");
        steering_queued_++;
        return;
    }

    add(Kind::User, line);
    if (starts_with(trim(line), "/")) {
        handle_slash(trim(line));
        return;
    }
    start_turn(line);
}

bool Tui::handle_mouse() {
    MEVENT me;
    if (getmouse(&me) != OK) return false;

    // Wheel up/down. BUTTON5 needs NCURSES_MOUSE_VERSION >= 2, which the
    // MacPorts build has, but guard anyway so this compiles against older
    // headers.
    if (me.bstate & BUTTON4_PRESSED) { scroll_by(3); return true; }
#ifdef BUTTON5_PRESSED
    if (me.bstate & BUTTON5_PRESSED) { scroll_by(-3); return true; }
#endif
    return false;
}

void Tui::handle_key(int ch) {
    if (approving_) {
        // Answering is line-based, exactly like every other input here.
        //
        // Single-letter hotkeys were tried and cannot work: at the moment the
        // first key arrives there is no way to tell "a" meaning approve-all from
        // the "A" that begins "Actually, stop and do X instead". The letter was
        // silently swallowed and the tool silently approved. So letters are
        // always text, and answering uses either a typed line or a control key
        // that cannot occur in prose.
        bool decided = false, allow = false;

        if (ch == KEY_MOUSE) { handle_mouse(); return; }
        if (ch == KEY_PPAGE) { scroll_by(10); return; }
        if (ch == KEY_NPAGE) { scroll_by(-10); return; }

        if (ch == 3) {                          // Ctrl+C: deny and cancel
            cancel_.store(true);
            decided = true;
            allow = false;
        } else if (ch == 25) {                  // Ctrl+Y: yes
            decided = true;
            allow = true;
        } else if (ch == 14 || ch == 27) {      // Ctrl+N or Esc: no
            decided = true;
            allow = false;
        } else if (ch == '\r' || ch == KEY_ENTER) {
            std::string answer = to_lower(trim(ed_.text));
            if (answer == "y" || answer == "yes") {
                ed_.clear();
                decided = true;
                allow = true;
            } else if (answer == "n" || answer == "no") {
                ed_.clear();
                decided = true;
                allow = false;
            } else if (answer == "a" || answer == "all" || answer == "always") {
                ed_.clear();
                cfg_.yolo = true;
                decided = true;
                allow = true;
                add(Kind::Info, "approving all tools for the rest of this session");
            } else if (!answer.empty()) {
                // Anything else is a steering message. The tool is still
                // waiting, so say so rather than leaving it looking hung.
                submit();
                add(Kind::Status,
                    "still waiting on the approval above -- y, n, or a then Enter");
                return;
            } else {
                return;   // bare Enter does nothing while a prompt is up
            }
        } else {
            // Every other key edits the line, so a correction can be composed
            // while the prompt is up.
            bool was = approving_;
            approving_ = false;
            handle_key(ch);
            approving_ = was;
            return;
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
        case KEY_MOUSE: handle_mouse(); return;
        case 3:                                  // Ctrl+C
            if (busy_.load()) {
                cancel_.store(true);
                status_text_ = "cancelling";
            } else if (!ed_.text.empty()) {
                ed_.clear();
            } else {
                add(Kind::Info, "Ctrl+D or /quit to exit");
            }
            return;
        case 4:                                  // Ctrl+D
            if (ed_.text.empty()) quit_.store(true);
            else ed_.del();
            return;
        case '\r':
        case KEY_ENTER:
            // Always submit: while busy this queues steering rather than
            // starting a second turn.
            submit();
            return;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            ed_.backspace();
            return;
        case KEY_DC:  ed_.del();   return;
        case KEY_LEFT:  ed_.left();  return;
        case KEY_RIGHT: ed_.right(); return;
        case KEY_HOME: case 1: ed_.home(); return;
        case KEY_END:  case 5: ed_.end();  return;
        case 11: ed_.kill_to_end(); return;      // Ctrl+K
        case 21: ed_.clear(); return;            // Ctrl+U
        case 23: ed_.kill_word(); return;        // Ctrl+W
        case KEY_UP:
            if (ed_.text.find('\n') == std::string::npos) ed_.hist_prev();
            else scroll_by(1);
            return;
        case KEY_DOWN:
            if (ed_.text.find('\n') == std::string::npos) ed_.hist_next();
            else scroll_by(-1);
            return;
        case KEY_SR:  scroll_by(1);  return;     // Shift+Up
        case KEY_SF:  scroll_by(-1); return;     // Shift+Down
        case KEY_PPAGE: {
            int h, w; getmaxyx(stdscr, h, w); (void)w;
            scroll_by(std::max(1, h / 2));
            return;
        }
        case KEY_NPAGE: {
            int h, w; getmaxyx(stdscr, h, w); (void)w;
            scroll_by(-std::max(1, h / 2));
            return;
        }
        case KEY_SHOME:                           // top of transcript
            scroll_ = static_cast<int>(wrapped_.size());
            follow_ = false;
            needs_redraw_ = true;
            return;
        case KEY_SEND:                            // back to the live tail
            scroll_ = 0;
            follow_ = true;
            needs_redraw_ = true;
            return;
        case 12:                                  // Ctrl+L
            dirty_ = true;
            clearok(stdscr, TRUE);
            needs_redraw_ = true;
            return;
        case KEY_RESIZE:
            dirty_ = true;
            needs_redraw_ = true;
            return;
        default:
            break;
    }

    // Printable input. Bytes >= 0x80 are UTF-8 continuation bytes arriving one
    // at a time from getch(); appending them in order reassembles the sequence.
    if (ch >= 32 && ch < 127) {
        ed_.insert(std::string(1, static_cast<char>(ch)));
        ed_.hist_pos = -1;
    } else if (ch >= 128 && ch <= 255) {
        ed_.insert(std::string(1, static_cast<char>(ch)));
        ed_.hist_pos = -1;
    } else if (ch == 9) {
        ed_.insert("  ");
    }
    needs_redraw_ = true;
}

int Tui::run() {
    utf8_ok_ = setup_locale();
    // A UTF-8 terminal is the normal case on anything connecting over SSH, so
    // default the nicer glyphs on when the locale supports them.
    if (utf8_ok_) cfg_.unicode = true;

    initscr();
    raw();
    nonl();          // keep Return (13) distinct from Ctrl+J (10)
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);
    scrollok(stdscr, FALSE);

    // Mouse reporting. mouseinterval(0) stops ncurses waiting to synthesise
    // click events, which keeps wheel scrolling responsive.
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
    mouseinterval(0);

    pal_.init(cfg_.color);

    {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "ppcode -- PowerPC Leopard build. /help for commands, "
                      "Ctrl+D to quit.\n"
                      "model: %s    cwd: %s    tools: %zu\n"
                      "display: %s, %s",
                      cfg_.model.c_str(), agent_.cwd().c_str(), tools_.size(),
                      render::depth_name(pal_.depth()).c_str(),
                      utf8_ok_ ? "UTF-8" : "ASCII (locale is not UTF-8)");
        add(Kind::Info, buf);
    }

    int tick = 0;
    while (!quit_.load()) {
        pump_events();

        int ch = getch();
        while (ch != ERR) {
            if (picker_.active) {
                handle_picker_key(ch);
            } else if (ch == 10 && !busy_.load()) {
                // Ctrl+J inserts a newline; Return arrives as 13 thanks to nonl().
                ed_.insert("\n");
            } else {
                handle_key(ch);
            }
            // Any consumed key can change what is on screen. Requesting the
            // redraw here rather than in each handler is what keeps editing
            // visible: handle_key has many early returns, and every one of them
            // that forgot this left the edit invisible until some other event
            // happened to trigger a repaint.
            needs_redraw_ = true;
            if (quit_.load()) break;
            ch = getch();
        }

        if (busy_.load() && ++tick % 2 == 0) {
            spinner_++;
            needs_redraw_ = true;
        }
        if (needs_redraw_) draw();
        napms(40);
    }

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

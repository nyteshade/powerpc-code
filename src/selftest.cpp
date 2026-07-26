// selftest.cpp -- offline checks for the parts that are easy to get subtly
// wrong (SSE framing, streaming tool-call assembly, the edit tool's uniqueness
// rule), plus an optional live network probe.
#include "selftest.hpp"

#include "agent.hpp"
#include "attach.hpp"
#include "common.hpp"
#include "config.hpp"
#include "envinfo.hpp"
#include "http.hpp"
#include "job.hpp"
#include "openrouter.hpp"
#include "plist.hpp"
#include "render.hpp"
#include "tools.hpp"
#include "xcodeproj.hpp"
#include "utf8.hpp"
#include "webtools.hpp"
#include "yaml.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode {
namespace {

int g_checks = 0, g_fails = 0;

void check(bool cond, const std::string& what) {
    g_checks++;
    if (!cond) {
        g_fails++;
        std::printf("  FAIL  %s\n", what.c_str());
    } else {
        std::printf("  ok    %s\n", what.c_str());
    }
}

void test_strings() {
    std::printf("[strings]\n");
    check(trim("  hi \n") == "hi", "trim");
    check(starts_with("foobar", "foo"), "starts_with");
    check(ends_with("foobar", "bar"), "ends_with");
    check(join(split("a,b,c", ','), "-") == "a-b-c", "split/join");
    check(split("a,b,", ',').size() == 2, "split drops trailing empty field");
    check(split("", ',').size() == 1, "split of empty string");
    check(to_lower("AbC") == "abc", "to_lower");
    check(count_occurrences("aXbXc", "X") == 2, "count_occurrences");

    std::string s = "hello world";
    check(replace_first(s, "world", "there") && s == "hello there", "replace_first");

    auto w = wrap_text("the quick brown fox jumps", 10);
    bool widths_ok = true;
    for (const auto& l : w) if (l.size() > 10) widths_ok = false;
    check(w.size() >= 3 && widths_ok, "wrap_text respects width");
    check(wrap_text("supercalifragilistic", 8).size() == 3, "wrap_text hard-splits long words");
    check(wrap_text("a\n\nb", 10).size() == 3, "wrap_text keeps blank lines");
    // Indentation and column alignment must survive -- this is a coding tool.
    check(wrap_text("    indented", 40)[0] == "    indented", "wrap_text keeps indentation");
    check(wrap_text("a    b", 40)[0] == "a    b", "wrap_text keeps interior spacing");
    {
        auto w = wrap_text("        deeply indented code line that is long", 20);
        check(w[0] == "        deeply", "wrap_text breaks at a space, keeping indent");
    }
    check(elide("abcdefghij", 7).size() == 7, "elide length");
}

void test_json_helpers() {
    std::printf("[json helpers]\n");
    json j = json::parse(R"({"s":"str","n":42,"f":1.5,"b":true,"nil":null})");
    check(jstr(j, "s") == "str", "jstr");
    check(jstr(j, "missing", "dflt") == "dflt", "jstr default");
    check(jstr(j, "nil", "dflt") == "dflt", "jstr treats null as absent");
    check(jint(j, "n") == 42, "jint");
    check(jnum(j, "f") == 1.5, "jnum");
    check(jbool(j, "b"), "jbool");
    check(jptr(j, "s") && !jptr(j, "zzz"), "jptr");
    check(json_preview(json::parse(R"({"a":"x\ny"})")).find('\n') == std::string::npos,
          "json_preview flattens newlines");
}

void test_sse_parser() {
    std::printf("[sse parser]\n");
    auto collect = [](const std::string& s, std::vector<std::string>* got) {
        http::SseParser p;
        auto h = [&](const http::SseEvent& e) { got->push_back(e.data); return true; };
        p.feed(s.data(), s.size(), h);
    };
    {
        std::vector<std::string> got;
        collect("data: one\n\ndata: two\n\n", &got);
        check(got.size() == 2 && got[0] == "one" && got[1] == "two", "two whole events");
    }
    {
        // The socket splits wherever it likes, so every boundary must work.
        http::SseParser p;
        std::vector<std::string> got;
        auto h = [&](const http::SseEvent& e) { got.push_back(e.data); return true; };
        std::string s = "data: hello\n\ndata: world\n\n";
        for (char c : s) p.feed(&c, 1, h);
        check(got.size() == 2 && got[0] == "hello" && got[1] == "world",
              "byte-at-a-time reassembly");
    }
    {
        std::vector<std::string> got;
        collect(": OPENROUTER PROCESSING\n\ndata: x\n\n", &got);
        check(got.size() == 1 && got[0] == "x", "comment lines ignored");
    }
    {
        std::vector<std::string> got;
        collect("data: a\ndata: b\n\n", &got);
        check(got.size() == 1 && got[0] == "a\nb", "multi-line data joined");
    }
    {
        std::vector<std::string> got;
        collect("data: crlf\r\n\r\n", &got);
        check(got.size() == 1 && got[0] == "crlf", "CRLF line endings");
    }
    {
        http::SseParser p;
        int n = 0;
        auto h = [&](const http::SseEvent&) { n++; return false; };
        std::string s = "data: a\n\ndata: b\n\n";
        bool cont = p.feed(s.data(), s.size(), h);
        check(!cont && n == 1, "handler can abort the stream");
    }
    {
        http::SseParser p;
        std::vector<std::string> got;
        auto h = [&](const http::SseEvent& e) { got.push_back(e.data); return true; };
        std::string s = "data: trailing";
        p.feed(s.data(), s.size(), h);
        check(got.empty(), "unterminated event not emitted early");
        p.finish(h);
        check(got.size() == 1 && got[0] == "trailing", "finish() flushes tail");
    }
}

// The trickiest part of the protocol: a tool call is spread across many chunks,
// with the name in the first and the arguments dribbling in afterwards.
void test_stream_assembler() {
    std::printf("[stream assembler]\n");
    {
        std::string text;
        StreamEvents ev;
        ev.on_text = [&](const std::string& s) { text += s; };
        StreamAssembler a(ev);
        a.feed(R"({"choices":[{"delta":{"content":"Hel"}}]})");
        a.feed(R"({"choices":[{"delta":{"content":"lo"}}]})");
        a.feed(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})");
        bool more = a.feed("[DONE]");
        Message m = a.take_message();
        check(!more, "[DONE] ends the stream");
        check(text == "Hello" && m.content == "Hello", "content deltas accumulate");
        check(a.finish_reason() == "stop", "finish_reason captured");
    }
    {
        StreamEvents ev;
        StreamAssembler a(ev);
        a.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read_file","arguments":""}}]}}]})");
        a.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"pa"}}]}}]})");
        a.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"x.txt\"}"}}]}}]})");
        a.feed(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})");
        Message m = a.take_message();
        check(m.tool_calls.size() == 1, "one tool call assembled");
        if (m.tool_calls.size() == 1) {
            check(m.tool_calls[0].id == "call_1", "tool call id");
            check(m.tool_calls[0].name == "read_file", "tool call name");
            check(m.tool_calls[0].arguments == R"({"path":"x.txt"})",
                  "arguments concatenated across chunks");
            check(m.tool_calls[0].args_json()["path"] == "x.txt", "arguments parse");
        }
    }
    {
        // Two calls interleaved by index.
        StreamEvents ev;
        StreamAssembler a(ev);
        a.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"f0","arguments":"{}"}}]}}]})");
        a.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"b","function":{"name":"f1","arguments":"{"}}]}}]})");
        a.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":1,"function":{"arguments":"}"}}]}}]})");
        Message m = a.take_message();
        check(m.tool_calls.size() == 2, "two interleaved tool calls");
        if (m.tool_calls.size() == 2)
            check(m.tool_calls[0].name == "f0" && m.tool_calls[1].name == "f1",
                  "tool calls ordered by index");
    }
    {
        StreamEvents ev;
        StreamAssembler a(ev);
        a.feed(R"({"usage":{"prompt_tokens":10,"completion_tokens":4,"total_tokens":14,"cost":0.0001},"choices":[]})");
        check(a.usage().total_tokens == 14, "usage from the final chunk");
        check(a.usage().prompt_tokens == 10, "usage prompt_tokens");
    }
    {
        StreamEvents ev;
        StreamAssembler a(ev);
        bool more = a.feed(R"({"error":{"message":"rate limited","code":429}})");
        check(!more && a.had_error(), "mid-stream error stops the stream");
        check(a.error().find("rate limited") != std::string::npos, "error message surfaced");
    }
    {
        // A malformed chunk should be skipped, not fatal.
        StreamEvents ev;
        std::string text;
        ev.on_text = [&](const std::string& s) { text += s; };
        StreamAssembler a(ev);
        a.feed("{not json");
        a.feed(R"({"choices":[{"delta":{"content":"ok"}}]})");
        check(text == "ok" && !a.had_error(), "malformed chunk skipped");
    }
}

void test_message_serialisation() {
    std::printf("[message json]\n");
    Message m = Message::assistant("");
    ToolCall tc;
    tc.id = "c1"; tc.name = "bash"; tc.arguments = R"({"command":"ls"})";
    m.tool_calls.push_back(tc);
    json j = m.to_json();
    check(j["role"] == "assistant", "assistant role");
    check(j.contains("content"), "content key present even when empty");
    check(j["tool_calls"][0]["function"]["name"] == "bash", "tool call serialised");
    check(j["tool_calls"][0]["type"] == "function", "tool call type");

    Message t = Message::tool_result("c1", "bash", "output");
    json tj = t.to_json();
    check(tj["role"] == "tool" && tj["tool_call_id"] == "c1", "tool result serialised");

    ToolSpec sp;
    sp.name = "x"; sp.description = "d"; sp.parameters = json::object();
    check(sp.to_json()["function"]["name"] == "x", "tool spec serialised");
}

void test_glob_and_shell() {
    std::printf("[shell]\n");
    CommandResult r = run_shell("echo hello && echo err >&2", ".", 10000);
    check(r.exit_code == 0, "shell exit code");
    check(r.output.find("hello") != std::string::npos, "captures stdout");
    check(r.output.find("err") != std::string::npos, "captures stderr");

    CommandResult f = run_shell("exit 3", ".", 10000);
    check(f.exit_code == 3, "propagates non-zero exit");

    // The timeout must actually kill the process group, not just give up on it.
    CommandResult t = run_shell("sleep 10", ".", 700);
    check(t.timed_out, "timeout fires");
    check(t.exit_code != 0, "timed-out command reports failure");
}

void test_tools() {
    std::printf("[tools]\n");
    ToolRegistry reg;
    reg.add_builtins();
    check(reg.size() >= 7, "builtins registered");
    check(reg.has("read_file") && reg.has("edit_file") && reg.has("bash"),
          "expected builtins present");

    std::string dir = "/tmp/ppcode-selftest";
    fs::remove_all(dir);
    fs::create_directories(dir);

    ToolContext ctx;
    ctx.cwd = dir;
    ctx.approve = nullptr;   // allow everything in the test

    // write -> read round trip
    ToolResult w = reg.call("write_file",
                            json{{"path", "a.txt"}, {"content", "one\ntwo\nthree\n"}}, ctx);
    check(!w.is_error, "write_file succeeds");

    ToolResult rd = reg.call("read_file", json{{"path", "a.txt"}}, ctx);
    check(!rd.is_error && rd.content.find("two") != std::string::npos, "read_file returns content");
    check(rd.content.find("     1\t") != std::string::npos, "read_file numbers lines");

    ToolResult off = reg.call("read_file", json{{"path", "a.txt"}, {"offset", 2}, {"limit", 1}}, ctx);
    check(off.content.find("two") != std::string::npos &&
          off.content.find("one") == std::string::npos, "read_file offset/limit");

    // edit uniqueness rule
    ToolResult e1 = reg.call("edit_file",
                             json{{"path", "a.txt"}, {"old_string", "two"}, {"new_string", "2"}}, ctx);
    check(!e1.is_error, "edit_file replaces unique string");

    ToolResult e2 = reg.call("edit_file",
                             json{{"path", "a.txt"}, {"old_string", "nope"}, {"new_string", "x"}}, ctx);
    check(e2.is_error, "edit_file errors when old_string is absent");

    reg.call("write_file", json{{"path", "b.txt"}, {"content", "x\nx\nx\n"}}, ctx);
    ToolResult e3 = reg.call("edit_file",
                             json{{"path", "b.txt"}, {"old_string", "x"}, {"new_string", "y"}}, ctx);
    check(e3.is_error && e3.content.find("3 times") != std::string::npos,
          "edit_file refuses an ambiguous match");

    ToolResult e4 = reg.call("edit_file",
                             json{{"path", "b.txt"}, {"old_string", "x"}, {"new_string", "y"},
                                  {"replace_all", true}}, ctx);
    check(!e4.is_error, "edit_file replace_all");

    // list / glob / grep
    ToolResult ls = reg.call("list_dir", json{{"path", "."}}, ctx);
    check(!ls.is_error && ls.content.find("a.txt") != std::string::npos, "list_dir");

    ToolResult g = reg.call("glob", json{{"pattern", "*.txt"}}, ctx);
    check(!g.is_error && g.content.find("a.txt") != std::string::npos, "glob matches");

    ToolResult gr = reg.call("grep", json{{"pattern", "three"}}, ctx);
    check(!gr.is_error && gr.content.find("a.txt") != std::string::npos, "grep finds match");

    ToolResult gr2 = reg.call("grep", json{{"pattern", "zzzz"}}, ctx);
    check(gr2.content.find("No matches") != std::string::npos, "grep reports no matches");

    // unknown tool must not throw
    ToolResult unk = reg.call("no_such_tool", json::object(), ctx);
    check(unk.is_error, "unknown tool returns an error");

    // approval gate
    ToolContext denied;
    denied.cwd = dir;
    denied.approve = [](const std::string&, ToolKind, const ToolPreview&) { return false; };
    ToolResult blocked = reg.call("write_file",
                                  json{{"path", "c.txt"}, {"content", "no"}}, denied);
    check(blocked.is_error, "approval gate blocks mutation");
    check(!fs::exists(dir + "/c.txt"), "blocked write did not touch the disk");

    ToolResult allowed_read = reg.call("read_file", json{{"path", "a.txt"}}, denied);
    check(!allowed_read.is_error, "read-only tools bypass the approval gate");

    fs::remove_all(dir);
}

void test_utf8() {
    std::printf("[utf8]\n");
    const std::string ascii = "hello";
    const std::string accent = "caf\xC3\xA9";                 // café
    const std::string cjk = "\xE6\x97\xA5\xE6\x9C\xAC";       // 日本
    const std::string emoji = "\xF0\x9F\x8E\x89";             // party popper

    check(utf8::width(ascii) == 5, "ascii width");
    check(utf8::width(accent) == 4, "2-byte sequence counts as one column");
    check(utf8::width(cjk) == 4, "CJK counts as two columns each");
    check(utf8::width(emoji) == 2, "emoji counts as two columns");
    check(utf8::valid(accent) && utf8::valid(cjk) && utf8::valid(emoji),
          "valid() accepts well-formed input");
    check(!utf8::valid("a\xFF\xFE"), "valid() rejects stray bytes");

    // The bug that caused the reported rendering artifacts: truncating by bytes
    // splits a sequence and paints garbage.
    check(utf8::truncate_to_width(accent, 3) == "caf",
          "truncate stops on a codepoint boundary");
    std::string t = utf8::truncate_to_width(cjk, 3);
    check(t == "\xE6\x97\xA5", "truncate never splits a wide char");
    check(utf8::valid(utf8::truncate_to_width(emoji, 1)),
          "truncating below a wide char yields valid output");

    check(utf8::step(accent, 0, 1) == 1, "step over 1-byte char");
    check(utf8::step(accent, 3, 1) == 5, "step over 2-byte char");
    check(utf8::step(accent, 5, -1) == 3, "step backwards over 2-byte char");
    check(utf8::floor_boundary(accent, 4) == 3, "floor_boundary snaps back");

    auto wrapped = utf8::wrap(cjk + cjk + cjk, 4);
    check(wrapped.size() == 3, "wrap counts columns, not bytes");
    for (const auto& l : wrapped) check(utf8::valid(l), "wrapped line is valid utf8");

    check(utf8::repair("a\xFFb").find("\xEF\xBF\xBD") != std::string::npos,
          "repair substitutes U+FFFD");
    check(utf8::sanitize("a\tb", 4) == "a    b", "sanitize expands tabs");
    // Octal escapes, not hex: "\x01b" would be read as the single byte 0x1B
    // because a hex escape consumes every hex digit that follows it.
    check(utf8::sanitize("a\001b").find("^A") != std::string::npos,
          "sanitize escapes control characters");
    check(utf8::sanitize("a\033b").find("^[") != std::string::npos,
          "sanitize escapes ESC so it cannot drive the terminal");
    check(utf8::elide(cjk + cjk + cjk, 6).size() > 0 &&
          utf8::valid(utf8::elide(cjk + cjk + cjk, 6)), "elide stays valid");
}

void test_yaml() {
    std::printf("[yaml]\n");
    auto ok_parse = [](const std::string& text, json* out) {
        std::string err;
        bool r = yaml::parse(text, out, &err);
        if (!r) std::printf("        (parse error: %s)\n", err.c_str());
        return r;
    };

    {
        json j;
        check(ok_parse("a: 1\nb: hello\nc: true\nd: 1.5\ne: ~", &j), "scalars parse");
        check(j["a"] == 1, "integer inferred");
        check(j["b"] == "hello", "string inferred");
        check(j["c"] == true, "bool inferred");
        check(j["d"] == 1.5, "float inferred");
        check(j["e"].is_null(), "~ is null");
    }
    {
        json j;
        check(ok_parse("outer:\n  inner: 2\n  deep:\n    x: y\n", &j), "nested maps");
        check(j["outer"]["inner"] == 2, "nested value");
        check(j["outer"]["deep"]["x"] == "y", "twice-nested value");
    }
    {
        json j;
        check(ok_parse("list:\n  - one\n  - two\n", &j), "block sequence");
        check(j["list"].is_array() && j["list"].size() == 2, "sequence length");
        check(j["list"][1] == "two", "sequence item");
    }
    {
        json j;
        check(ok_parse("flow: [a, b, c]\nmap: {x: 1, y: 2}\n", &j), "flow collections");
        check(j["flow"].size() == 3 && j["flow"][2] == "c", "flow sequence");
        check(j["map"]["y"] == 2, "flow mapping");
    }
    {
        // A mapping that begins on the dash line -- the fiddliest common case.
        json j;
        check(ok_parse("items:\n  - name: a\n    value: 1\n  - name: b\n    value: 2\n",
                       &j),
              "sequence of mappings");
        check(j["items"].size() == 2, "two mappings in sequence");
        check(j["items"][0]["name"] == "a" && j["items"][1]["value"] == 2,
              "dash-line mapping keys");
    }
    {
        json j;
        check(ok_parse("# leading comment\nk: v  # trailing\nq: \"has # hash\"\n", &j),
              "comments stripped");
        check(j["k"] == "v", "trailing comment removed");
        check(j["q"] == "has # hash", "hash inside quotes preserved");
    }
    {
        json j;
        check(ok_parse("text: |\n  line one\n  line two\n", &j), "literal block scalar");
        check(j["text"] == "line one\nline two\n", "literal block keeps newlines");
    }
    {
        json j;
        check(ok_parse("text: >-\n  folded one\n  folded two\n", &j), "folded block scalar");
        check(j["text"] == "folded one folded two", "folded block joins lines");
    }
    {
        json j;
        std::string err;
        check(!yaml::parse("a:\n\tb: 1\n", &j, &err), "tab indentation rejected");
        check(err.find("tab") != std::string::npos, "tab error explains itself");
    }
    {
        std::string f, b, err;
        check(yaml::split_frontmatter("---\na: 1\n---\nbody here\n", &f, &b, &err),
              "frontmatter split");
        check(trim(f) == "a: 1", "frontmatter content");
        check(trim(b) == "body here", "body content");
    }
    {
        std::string f, b, err;
        check(yaml::split_frontmatter("no frontmatter\n", &f, &b, &err),
              "document without frontmatter");
        check(f.empty() && trim(b) == "no frontmatter", "all body");
    }
    {
        std::string f, b, err;
        check(!yaml::split_frontmatter("---\na: 1\nnever closed\n", &f, &b, &err),
              "unclosed frontmatter is an error");
    }
}

void test_job() {
    std::printf("[job files]\n");
    const std::string text =
        "---\n"
        "name: demo\n"
        "model: z-ai/glm-5.2\n"
        "models: [deepseek/deepseek-v4-pro, moonshotai/kimi-k3]\n"
        "provider:\n"
        "  sort: throughput\n"
        "  order: [together, fireworks]\n"
        "  allow_fallbacks: false\n"
        "  data_collection: deny\n"
        "reasoning:\n"
        "  effort: high\n"
        "temperature: 0.3\n"
        "max_turns: 12\n"
        "web_search: true\n"
        "tools:\n"
        "  allow: [bash, edit_file]\n"
        "  deny: [file_op]\n"
        "environment:\n"
        "  detail: brief\n"
        "  knowledge: false\n"
        "---\n"
        "Do the thing.\n";

    job::Spec spec;
    std::vector<std::string> warn;
    std::string err;
    check(job::parse_text(text, &spec, &warn, &err),
          "job parses" + (err.empty() ? "" : ": " + err));
    check(spec.name == "demo", "job name");
    check(spec.model == "z-ai/glm-5.2", "job model");
    check(spec.model_fallbacks.size() == 2, "fallback models");
    check(spec.provider["sort"] == "throughput", "provider sort");
    check(spec.provider["order"].size() == 2, "provider order list");
    check(spec.provider["allow_fallbacks"] == false, "provider allow_fallbacks");
    check(spec.reasoning["effort"] == "high", "reasoning effort");
    check(spec.temperature && *spec.temperature == 0.3, "temperature");
    check(spec.max_turns && *spec.max_turns == 12, "max_turns");
    check(spec.web_search && *spec.web_search, "web_search");
    check(spec.allow_tools.size() == 2 && spec.deny_tools.size() == 1, "tool lists");
    check(spec.env_detail == "brief", "environment detail");
    check(spec.knowledge && !*spec.knowledge, "knowledge disabled");
    check(trim(spec.prompt) == "Do the thing.", "prompt body");

    // "models:" alone should promote the first entry to primary.
    {
        job::Spec s2;
        std::string e2;
        check(job::parse_text("---\nmodels: [a/one, b/two]\n---\nbody\n", &s2, nullptr,
                              &e2),
              "models-only job parses");
        check(s2.model == "a/one" && s2.model_fallbacks.size() == 1,
              "first model promoted to primary");
    }
    // An empty body is a usage error, not a silent no-op.
    {
        job::Spec s3;
        std::string e3;
        check(!job::parse_text("---\nmodel: x/y\n---\n\n", &s3, nullptr, &e3),
              "empty job body rejected");
    }
    // Unknown keys must be reported rather than silently dropped.
    {
        job::Spec s4;
        std::vector<std::string> w4;
        std::string e4;
        job::parse_text("---\nmodle: typo\n---\nbody\n", &s4, &w4, &e4);
        bool mentioned = false;
        for (const std::string& w : w4)
            if (w.find("modle") != std::string::npos) mentioned = true;
        check(mentioned, "typo in frontmatter key warned about");
    }

    // Config application
    {
        Config cfg;
        job::apply_to_config(spec, &cfg);
        check(cfg.model == "z-ai/glm-5.2", "config model applied");
        check(cfg.max_turns == 12, "config max_turns applied");
        check(cfg.web_search, "config web_search applied");
        check(cfg.provider["sort"] == "throughput", "config provider applied");
    }
}

void test_multimodal() {
    std::printf("[multimodal]\n");
    check(attach::is_image_mime("image/png"), "image mime detected");
    check(!attach::is_image_mime("text/plain"), "non-image mime");

    std::string dir = "/tmp/ppcode-attach-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // A minimal but real PNG header so magic-byte sniffing has something to see.
    std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, '\0');
    write_file_text(dir + "/pic.png", png, nullptr);
    write_file_text(dir + "/notes.txt", "hello notes", nullptr);

    check(attach::detect_mime(dir + "/pic.png") == "image/png",
          "png detected by magic bytes");
    check(starts_with(attach::detect_mime(dir + "/notes.txt"), "text/"),
          "text detected by extension");

    {
        attach::Loaded l = attach::load("pic.png", "auto", "auto", true, dir);
        check(l.ok, "image loads for a vision model");
        check(l.part.type == ContentPart::Type::ImageUrl, "image becomes an image part");
        check(starts_with(l.part.url, "data:image/png;base64,"), "image becomes a data URI");
    }
    {
        // Degrading rather than failing is the point here.
        attach::Loaded l = attach::load("pic.png", "auto", "auto", false, dir);
        check(l.ok, "image attachment degrades for a text-only model");
        check(l.part.type == ContentPart::Type::Text, "degraded to a text description");
        check(l.part.text.find("cannot see images") != std::string::npos,
              "degradation explains itself");
    }
    {
        attach::Loaded l = attach::load("notes.txt", "auto", "auto", true, dir);
        check(l.ok && l.part.type == ContentPart::Type::Text, "text file inlined");
        check(l.part.text.find("hello notes") != std::string::npos, "text content present");
    }
    {
        attach::Loaded l = attach::load("missing.png", "auto", "auto", true, dir);
        check(!l.ok, "missing attachment reports an error");
    }
    {
        attach::Loaded l = attach::load("https://example.com/x.png", "auto", "auto",
                                        true, dir);
        check(l.ok && l.part.url == "https://example.com/x.png",
              "remote image passed through as a URL");
    }

    // Message serialisation with parts
    {
        Message m;
        m.role = "user";
        m.parts.push_back(ContentPart::make_text("look"));
        m.parts.push_back(ContentPart::make_image("data:image/png;base64,AAA", "high"));
        json j = m.to_json();
        check(j["content"].is_array(), "multi-part content is an array");
        check(j["content"][0]["type"] == "text", "text part serialised");
        check(j["content"][1]["type"] == "image_url", "image part serialised");
        check(j["content"][1]["image_url"]["detail"] == "high", "image detail sent");
        check(m.display_text().find("[image:") != std::string::npos,
              "display_text hides the data URI");
    }

    check(base64_encode("") == "", "base64 of empty string");
    check(base64_encode("f") == "Zg==", "base64 one byte");
    check(base64_encode("fo") == "Zm8=", "base64 two bytes");
    check(base64_encode("foo") == "Zm9v", "base64 three bytes");
    check(base64_encode("hello world") == "aGVsbG8gd29ybGQ=", "base64 longer string");

    fs::remove_all(dir);
}

void test_render() {
    std::printf("[render]\n");
    check(render::rgb_to_256({0, 0, 0}) == 16, "rgb_to_256 black");
    check(render::rgb_to_256({255, 255, 255}) == 231, "rgb_to_256 white");
    int grey = render::rgb_to_256({128, 128, 128});
    check(grey >= 232 && grey <= 255, "mid grey maps to the grey ramp");
    check(render::rgb_to_16({200, 0, 0}) == 1, "rgb_to_16 red");
    check(render::rgb_to_16({0, 170, 170}) == 6, "rgb_to_16 cyan");

    check(render::detect_depth(0, false) == render::ColorDepth::Mono, "no colour");
    check(render::detect_depth(8, true) == render::ColorDepth::Ansi16, "16 colour");
    check(render::detect_depth(256, true) == render::ColorDepth::Ansi256, "256 colour");
    check(render::detect_depth(0x10000, true) == render::ColorDepth::TrueColor,
          "direct colour terminfo");

    {
        auto lines = render::markdown("# Title\n\nSome **bold** text.\n", 60, false);
        check(!lines.empty(), "markdown produces lines");
        bool found_heading = false, found_bold = false;
        for (const auto& l : lines)
            for (const auto& s : l.spans) {
                if (s.style == render::Style::Heading1) found_heading = true;
                if (s.style == render::Style::Bold) found_bold = true;
            }
        check(found_heading, "heading styled");
        check(found_bold, "bold styled");
    }
    {
        auto lines = render::markdown("- one\n- two\n", 60, false);
        bool gutter = false;
        for (const auto& l : lines) if (!l.gutter.empty()) gutter = true;
        check(gutter, "list items get a gutter");
    }
    {
        auto lines = render::markdown("```cpp\nint x = 42; // note\n```\n", 60, false);
        bool kw = false, num = false, com = false;
        for (const auto& l : lines)
            for (const auto& s : l.spans) {
                if (s.style == render::Style::Type) kw = true;
                if (s.style == render::Style::Number) num = true;
                if (s.style == render::Style::Comment) com = true;
            }
        check(kw, "cpp type highlighted");
        check(num, "number highlighted");
        check(com, "comment highlighted");
    }
    check(render::language_supported("python"), "python highlighter present");
    check(render::language_supported("sh"), "shell highlighter present");
    check(render::language_supported("diff"), "diff highlighter present");
    check(!render::language_supported("brainfuck"), "unknown language reported");
    {
        // A code block must never be re-flowed in a way that loses text.
        auto lines = render::highlight("    indented = 1", "python", 80);
        check(!lines.empty() && lines[0].plain().find("    indented") == 0,
              "code keeps indentation");
    }
}

void test_web() {
    std::printf("[web]\n");
    check(web::html_to_text("<p>Hello <b>world</b></p>").find("Hello") !=
              std::string::npos,
          "html_to_text extracts text");
    check(web::html_to_text("<script>var x=1;</script><p>ok</p>").find("var x") ==
              std::string::npos,
          "script contents dropped");
    check(web::html_to_text("<p>a&amp;b &lt;c&gt;</p>").find("a&b <c>") !=
              std::string::npos,
          "entities decoded");
    check(web::html_to_text("<p>&#65;&#x42;</p>").find("AB") != std::string::npos,
          "numeric entities decoded");
    check(web::html_title("<html><head><title>Hi There</title></head></html>") ==
              "Hi There",
          "title extracted");
    {
        std::string txt = web::html_to_text("<div>one</div><div>two</div>");
        check(txt.find("one") != std::string::npos &&
                  txt.find("two") != std::string::npos,
              "block elements separated");
    }
    {
        web::SearchConfig c;
        check(c.resolve() == web::SearchBackend::Reference,
              "no keys falls back to reference search");
        check(!c.availability_note().empty(), "availability note explains the situation");
        c.tavily_key = "x";
        check(c.resolve() == web::SearchBackend::Tavily, "tavily preferred when keyed");
    }
    {
        bool ok = false;
        check(web::backend_from_string("brave", &ok) == web::SearchBackend::Brave && ok,
              "backend parsed");
        web::backend_from_string("nonsense", &ok);
        check(!ok, "bad backend name reported");
    }
}

void test_envinfo() {
    std::printf("[envinfo]\n");
    bool ok = false;
    check(envinfo::detail_from_string("full", &ok) == envinfo::Detail::Full && ok,
          "detail parsed");
    envinfo::detail_from_string("nope", &ok);
    check(!ok, "bad detail name reported");
    check(envinfo::detail_to_string(envinfo::Detail::Brief) == "brief",
          "detail round-trips");

    // Detail must scale with the context window: a small model should get less.
    envinfo::Probe p;
    p.ok = true;
    p.hostname = "test";
    p.model_name = "Power Mac G5";
    p.machine_model = "PowerMac11,2";
    p.os_name = "Mac OS X Server";
    p.os_version = "10.5.8";
    p.cpu_count = 2;
    p.memory_bytes = 17179869184ULL;
    p.big_endian = true;
    p.ports_prefix = "/opt/local";
    for (int i = 0; i < 300; i++)
        p.ports.push_back("port-" + std::to_string(i) + " @1.0.0");
    p.caveats.push_back("a caveat that takes up some room in the rendering");

    check(!envinfo::render(p, envinfo::Detail::Minimal).empty(), "minimal renders");
    check(envinfo::render(p, envinfo::Detail::None).empty(), "none renders nothing");

    size_t minimal = envinfo::render(p, envinfo::Detail::Minimal).size();
    size_t brief = envinfo::render(p, envinfo::Detail::Brief).size();
    size_t full = envinfo::render(p, envinfo::Detail::Full).size();
    check(minimal < brief && brief < full, "detail levels grow monotonically");

    check(envinfo::choose_detail(p, 1000000) == envinfo::Detail::Full,
          "huge context gets full detail");
    envinfo::Detail small = envinfo::choose_detail(p, 16000);
    check(small == envinfo::Detail::Minimal || small == envinfo::Detail::Brief,
          "small context gets reduced detail");
    check(envinfo::choose_detail(p, 4000) != envinfo::Detail::Full,
          "tiny context never gets full detail");
    check(envinfo::render(p, envinfo::Detail::Full).find("big-endian") !=
              std::string::npos,
          "endianness stated");
    check(envinfo::estimate_tokens("abcd") >= 1, "token estimate");
}

void test_plist_and_xcode() {
    std::printf("[plist / xcode]\n");

    // A miniature but structurally faithful pbxproj.
    const std::string src =
        "// !$*UTF8*$!\n"
        "{\n"
        "\tarchiveVersion = 1;\n"
        "\tobjectVersion = 45;\n"
        "\trootObject = AAAA /* Project object */;\n"
        "\tobjects = {\n"
        "\t\tAAAA /* Project object */ = {\n"
        "\t\t\tisa = PBXProject;\n"
        "\t\t\tcompatibilityVersion = \"Xcode 3.1\";\n"
        "\t\t\tmainGroup = GGGG;\n"
        "\t\t\tbuildConfigurationList = PCFG;\n"
        "\t\t\ttargets = ( TTTT /* Demo */, );\n"
        "\t\t};\n"
        "\t\tGGGG = { isa = PBXGroup; children = ( FFFF /* main.m */, ); "
        "sourceTree = \"<group>\"; };\n"
        "\t\tFFFF /* main.m */ = { isa = PBXFileReference; path = main.m; "
        "lastKnownFileType = sourcecode.c.objc; sourceTree = \"<group>\"; };\n"
        "\t\tBBBB /* main.m in Sources */ = { isa = PBXBuildFile; "
        "fileRef = FFFF /* main.m */; };\n"
        "\t\tSRCP = { isa = PBXSourcesBuildPhase; files = ( BBBB, ); };\n"
        "\t\tFWKP = { isa = PBXFrameworksBuildPhase; files = ( ); };\n"
        "\t\tTTTT /* Demo */ = {\n"
        "\t\t\tisa = PBXNativeTarget;\n"
        "\t\t\tname = Demo;\n"
        "\t\t\tproductType = \"com.apple.product-type.application\";\n"
        "\t\t\tbuildPhases = ( SRCP, FWKP, );\n"
        "\t\t\tbuildConfigurationList = TCFG;\n"
        "\t\t};\n"
        "\t\tTCFG = { isa = XCConfigurationList; buildConfigurations = ( TDBG, ); };\n"
        "\t\tTDBG = { isa = XCBuildConfiguration; name = Debug; "
        "buildSettings = { PRODUCT_NAME = Demo; }; };\n"
        "\t\tPCFG = { isa = XCConfigurationList; buildConfigurations = ( PDBG, ); };\n"
        "\t\tPDBG = { isa = XCBuildConfiguration; name = Debug; "
        "buildSettings = { SDKROOT = macosx10.5; }; };\n"
        "\t};\n"
        "}\n";

    std::string err;
    plist::ValuePtr root = plist::parse(src, &err);
    check(root != nullptr, "pbxproj parses" + (err.empty() ? "" : ": " + err));
    if (!root) return;

    check(root->get_string("objectVersion") == "45", "scalar read");
    check(root->get_string("rootObject") == "AAAA", "reference read");
    plist::ValuePtr objects = root->get("objects");
    check(objects && objects->is_dict(), "objects is a dict");
    check(objects->get("TTTT")->get_string("name") == "Demo", "nested dict read");
    check(objects->get("SRCP")->get("files")->items.size() == 1, "array read");
    check(objects->get("FWKP")->get("files")->items.empty(), "empty array read");

    // Quoted strings and the /* comment */ annotations.
    check(objects->get("AAAA")->get_string("compatibilityVersion") == "Xcode 3.1",
          "quoted string unescaped");
    check(objects->get("FFFF")->comment == "main.m", "comment captured");

    // Round-trip: serialise, re-parse, and confirm the structure survives.
    std::string out = plist::serialize(root, true);
    check(starts_with(out, "// !$*UTF8*$!"), "serialised with the UTF8 header");
    check(out.find("/* Begin PBXFileReference section */") != std::string::npos,
          "objects grouped into isa sections");

    std::string err2;
    plist::ValuePtr again = plist::parse(out, &err2);
    check(again != nullptr, "round-trip re-parses" + (err2.empty() ? "" : ": " + err2));
    if (again) {
        plist::ValuePtr o2 = again->get("objects");
        check(o2 && o2->get("TTTT")->get_string("name") == "Demo",
              "round-trip preserves target name");
        check(o2->get("PDBG")->get("buildSettings")->get_string("SDKROOT") ==
                  "macosx10.5",
              "round-trip preserves build settings");
        check(o2->entries.size() == objects->entries.size(),
              "round-trip preserves object count");
    }

    check(plist::needs_quoting("Xcode 3.1"), "space forces quoting");
    check(!plist::needs_quoting("macosx10.5"), "plain token needs no quotes");
    check(plist::quote("a\"b") == "\"a\\\"b\"", "quote escapes a double quote");

    check(xcode::file_type_for("x.m") == "sourcecode.c.objc", "file type for .m");
    check(xcode::file_type_for("x.cpp") == "sourcecode.cpp.cpp", "file type for .cpp");
    check(xcode::file_type_for("x.xib") == "file.xib", "file type for .xib");
    check(xcode::is_source_extension("a.m") && !xcode::is_source_extension("a.h"),
          "headers are not compiled");

    // Exercise the mutations against a temporary project on disk.
    std::string dir = "/tmp/ppcode-xcode-test/Demo.xcodeproj";
    fs::remove_all("/tmp/ppcode-xcode-test");
    fs::create_directories(dir);
    write_file_text(dir + "/project.pbxproj", src, nullptr);

    xcode::Project p;
    check(p.load("/tmp/ppcode-xcode-test", &err), "project loads from a directory");
    check(p.targets().size() == 1, "one target found");
    if (!p.targets().empty()) {
        check(p.targets()[0].name == "Demo", "target name");
        check(p.targets()[0].source_files.size() == 1, "target sources listed");
    }
    check(p.describe().find("Demo") != std::string::npos, "describe mentions the target");

    check(p.add_file("Extra.m", "Demo", "", &err),
          "add_file succeeds" + (err.empty() ? "" : ": " + err));
    check(!p.add_file("Extra.m", "Demo", "", &err),
          "adding the same file twice is refused");
    check(p.add_framework("WebKit", "Demo", &err),
          "add_framework succeeds" + (err.empty() ? "" : ": " + err));
    check(p.set_setting("ARCHS", "ppc", "Demo", "", false, &err),
          "set_setting succeeds" + (err.empty() ? "" : ": " + err));
    check(p.save(&err), "project saves" + (err.empty() ? "" : ": " + err));

    // Reload and confirm every edit is really there and still consistent.
    xcode::Project p2;
    check(p2.load("/tmp/ppcode-xcode-test", &err), "modified project reloads");
    std::vector<xcode::Target> ts = p2.targets();
    check(!ts.empty(), "target survives the rewrite");
    if (!ts.empty()) {
        bool has_extra = false;
        for (const std::string& f : ts[0].source_files) if (f == "Extra.m") has_extra = true;
        check(has_extra, "added file is in the sources phase");

        bool has_fw = false;
        for (const std::string& f : ts[0].frameworks)
            if (f.find("WebKit") != std::string::npos) has_fw = true;
        check(has_fw, "framework is in the frameworks phase");

        bool has_archs = false;
        for (const xcode::BuildConfig& c : ts[0].configs)
            for (const auto& [k, v] : c.settings)
                if (k == "ARCHS" && v == "ppc") has_archs = true;
        check(has_archs, "build setting is present after reload");
    }
    check(fs::exists(dir + "/project.pbxproj.ppcode-bak"), "a backup was written");

    fs::remove_all("/tmp/ppcode-xcode-test");
}

void test_config() {
    std::printf("[config]\n");
    std::vector<std::string> warn;
    Config c = Config::load("/nonexistent/path/config.json", &warn);
    check(c.model == "anthropic/claude-sonnet-5", "default model");
    check(!c.default_system_prompt().empty(), "system prompt non-empty");
    check(c.effective_system_prompt() == c.default_system_prompt(), "effective falls back");
    check(!Config::default_path().empty(), "default_path");
    std::printf("  info  api key %s\n",
                c.api_key.empty() ? "NOT FOUND in env" : "found in env");
}

void test_agent_offline() {
    std::printf("[agent]\n");
    Config cfg;
    cfg.api_key = "";              // force the no-key path
    Client client(cfg);
    ToolRegistry reg;
    reg.add_builtins();
    Agent a(client, reg, cfg);

    check(a.history().size() == 1 && a.history()[0].role == "system",
          "system prompt seeded");

    // Regression: set_cwd used to leave the old directory named in the system
    // prompt, which sent the model editing files in the wrong tree.
    a.set_cwd("/tmp/ppcode-cwd-check");
    check(a.history().size() == 1 && a.history()[0].role == "system",
          "set_cwd keeps exactly one system message");
    check(a.history()[0].content.find("/tmp/ppcode-cwd-check") != std::string::npos,
          "set_cwd updates the directory in the system prompt");

    Agent::Events ev;
    Agent::RunResult r = a.run("hello", ev);
    check(!r.ok && r.error.find("API key") != std::string::npos,
          "missing API key reported clearly");

    // Round-trip the conversation.
    json snapshot = a.to_json();
    Agent b(client, reg, cfg);
    std::string err;
    check(b.from_json(snapshot, &err), "session restores from json");
    check(b.history().size() == a.history().size(), "restored history length matches");
}

void test_network() {
    std::printf("[network]\n");
    Config cfg = Config::load("", nullptr);
    if (cfg.api_key.empty()) {
        std::printf("  skip  no API key in environment\n");
        return;
    }
    Client client(cfg);
    std::string err;
    std::vector<ModelInfo> models = client.list_models(&err);
    check(!models.empty(), "list_models returned entries" + (err.empty() ? "" : ": " + err));
    if (!models.empty())
        std::printf("  info  %zu models\n", models.size());

    double credits = 0;
    if (client.get_credits(&credits, nullptr))
        std::printf("  info  credits remaining: %.4f\n", credits);
}

} // namespace

int run_selftest(bool with_network) {
    g_checks = 0;
    g_fails = 0;
    std::printf("ppcode self-test -- %s\n\n", http::version_string().c_str());
    test_strings();
    test_json_helpers();
    test_utf8();
    test_yaml();
    test_sse_parser();
    test_stream_assembler();
    test_message_serialisation();
    test_glob_and_shell();
    test_tools();
    test_job();
    test_multimodal();
    test_render();
    test_web();
    test_plist_and_xcode();
    test_envinfo();
    test_config();
    test_agent_offline();
    if (with_network) test_network();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails;
}

} // namespace ppcode

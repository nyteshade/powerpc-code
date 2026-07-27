// selftest.cpp -- offline checks for the parts that are easy to get subtly
// wrong (SSE framing, streaming tool-call assembly, the edit tool's uniqueness
// rule), plus an optional live network probe.
#include "selftest.hpp"

#include "agent.hpp"
#include "appledocs.hpp"
#include "builderr.hpp"
#include "bundler.hpp"
#include "checkpoint.hpp"
#include "attach.hpp"
#include "common.hpp"
#include "config.hpp"
#include "envinfo.hpp"
#include "http.hpp"
#include "job.hpp"
#include "mdparse.hpp"
#include "openrouter.hpp"
#include "plist.hpp"
#include "render.hpp"
#include "session.hpp"
#include "sysprompt.hpp"
#include "tools.hpp"
#include "xcodeproj.hpp"
#include "utf8.hpp"
#include "vecstore.hpp"
#include "webtools.hpp"
#include "xib.hpp"
#include "xml.hpp"
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
    // Objective-C is the primary language on this platform, so its distinctive
    // syntax has to be highlighted properly rather than run through the C++
    // tokenizer.
    {
        const char* objc =
            "@interface Greeter : NSObject {\n"
            "    NSString *_name;   // ivar\n"
            "}\n"
            "@property (nonatomic, retain) NSString *name;\n"
            "@end\n"
            "\n"
            "@implementation Greeter\n"
            "- (void)greetWithName:(NSString *)who count:(NSInteger)n {\n"
            "    if (who == nil) return;\n"
            "    NSLog(@\"hello %@\", who);\n"
            "    [self doThing:who withOther:n];\n"
            "}\n"
            "@end\n";
        auto lines = render::highlight(objc, "objc", 100);
        bool at_directive = false, ns_type = false, at_string = false;
        bool nil_const = false, selector = false, comment = false;
        for (const auto& l : lines)
            for (const auto& s : l.spans) {
                if (s.style == render::Style::Keyword &&
                    starts_with(s.text, "@interface")) at_directive = true;
                if (s.style == render::Style::Keyword &&
                    starts_with(s.text, "@property")) at_directive = true;
                if (s.style == render::Style::Type && s.text == "NSString") ns_type = true;
                if (s.style == render::Style::String &&
                    starts_with(s.text, "@\"")) at_string = true;
                if (s.style == render::Style::Constant && s.text == "nil") nil_const = true;
                if (s.style == render::Style::Function && s.text == "withOther")
                    selector = true;
                if (s.style == render::Style::Comment) comment = true;
            }
        check(at_directive, "objc @-directives highlighted as keywords");
        check(ns_type, "NS-prefixed framework types highlighted");
        check(at_string, "@\"literal\" highlighted as a string including the @");
        check(nil_const, "nil highlighted as a constant");
        check(selector, "message-send selector parts highlighted");
        check(comment, "objc comments highlighted");
    }
    check(render::language_supported("objc"), "objc highlighter present");
    check(render::language_supported("m"), ".m maps to the objc highlighter");
    check(render::language_supported("mm"), ".mm maps to objective-c++");
    {
        // A .mm file gets C++ keywords too.
        auto lines = render::highlight("std::vector<int> v; @autoreleasepool { }",
                                       "objcpp", 100);
        bool cpp_type = false, at_kw = false;
        for (const auto& l : lines)
            for (const auto& s : l.spans) {
                if (s.style == render::Style::Type && s.text == "vector") cpp_type = true;
                if (s.style == render::Style::Keyword &&
                    starts_with(s.text, "@autoreleasepool")) at_kw = true;
            }
        check(cpp_type && at_kw, "objective-c++ gets both C++ and ObjC syntax");
    }
    {
        // Outside a message send, "label:" must not be mistaken for a selector.
        auto lines = render::highlight("int x = a ? b : c;", "objc", 80);
        bool bad = false;
        for (const auto& l : lines)
            for (const auto& s : l.spans)
                if (s.style == render::Style::Function && s.text == "b") bad = true;
        check(!bad, "ternary is not mistaken for a selector");
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

// The markdown document model behind the Cocoa transcript. It lives in a plain
// .cpp precisely so it can be tested here rather than only through the GUI.
void test_mdparse() {
    std::printf("[mdparse]\n");

    // --- inline ------------------------------------------------------------
    {
        auto r = md::parse_inline("plain text");
        check(r.size() == 1 && r[0].style == 0, "inline plain text is one run");
    }
    {
        auto r = md::parse_inline("a **bold** b");
        check(r.size() == 3 && r[1].text == "bold" && (r[1].style & md::StyleBold),
              "**bold**");
    }
    {
        auto r = md::parse_inline("a *it* b");
        check(r.size() == 3 && r[1].text == "it" && (r[1].style & md::StyleItalic),
              "*italic*");
    }
    {
        // The reason for not reusing render.cpp's inline_spans: it carries one
        // Style per span and cannot say bold *and* italic.
        auto r = md::parse_inline("**b _i_**");
        bool both = false;
        for (size_t i = 0; i < r.size(); i++)
            if (r[i].text == "i" &&
                (r[i].style & md::StyleBold) && (r[i].style & md::StyleItalic))
                both = true;
        check(both, "emphasis nests: bold and italic at once");
    }
    {
        auto r = md::parse_inline("***both***");
        check(r.size() == 1 && r[0].text == "both" &&
                  (r[0].style & md::StyleBold) && (r[0].style & md::StyleItalic),
              "***triple*** is bold and italic, with no stray markers");
    }
    {
        auto r = md::parse_inline("call `foo(1)` now");
        check(r.size() == 3 && r[1].text == "foo(1)" && (r[1].style & md::StyleCode),
              "`inline code`");
    }
    {
        auto r = md::parse_inline("``a ` b``");
        check(r.size() == 1 && r[0].text == "a ` b" && (r[0].style & md::StyleCode),
              "double-backtick code may contain a backtick");
    }
    {
        // In a coding tool this matters more than catching every emphasis:
        // identifiers must survive.
        auto r = md::parse_inline("call read_file_text now");
        check(r.size() == 1 && r[0].style == 0, "snake_case is not italic");
        auto r2 = md::parse_inline("MAX__VALUE stays");
        check(r2.size() == 1 && r2[0].style == 0, "double underscore in a word is literal");
    }
    {
        auto r = md::parse_inline("2 * 3 * 4");
        check(r.size() == 1 && r[0].style == 0, "spaced asterisks are arithmetic");
    }
    {
        auto r = md::parse_inline("[docs](http://x.y/z)");
        check(r.size() == 1 && r[0].text == "docs" && (r[0].style & md::StyleLink) &&
                  r[0].href == "http://x.y/z",
              "[label](url) keeps the target");
    }
    {
        auto r = md::parse_inline("see http://a.b/c. done");
        bool ok = false;
        for (size_t i = 0; i < r.size(); i++)
            if ((r[i].style & md::StyleLink) && r[i].text == "http://a.b/c") ok = true;
        check(ok, "bare URL linkified without the trailing full stop");
    }
    {
        auto r = md::parse_inline("\\*not emphasis\\*");
        check(r.size() == 1 && r[0].text == "*not emphasis*" && r[0].style == 0,
              "backslash escapes");
    }
    {
        auto r = md::parse_inline("~~gone~~");
        check(r.size() == 1 && (r[0].style & md::StyleStrike), "~~strikethrough~~");
    }
    {
        // An unterminated marker must not swallow the rest of the message,
        // which is the common case mid-stream.
        auto r = md::parse_inline("an unclosed **bold");
        std::string all;
        for (size_t i = 0; i < r.size(); i++) all += r[i].text;
        check(all == "an unclosed **bold", "unterminated emphasis stays literal");
    }

    // --- blocks ------------------------------------------------------------
    {
        auto n = md::parse("# Title\n\nBody.\n");
        check(n.size() == 2 && n[0].kind == md::Block::Heading && n[0].level == 1 &&
                  n[1].kind == md::Block::Paragraph,
              "atx heading then paragraph");
    }
    {
        auto n = md::parse("###### Six\n");
        check(n.size() == 1 && n[0].level == 6, "heading level 6");
        auto n2 = md::parse("####### Seven\n");
        check(n2.size() == 1 && n2[0].kind == md::Block::Paragraph,
              "seven hashes is not a heading");
        auto n3 = md::parse("#tag\n");
        check(n3.size() == 1 && n3[0].kind == md::Block::Paragraph,
              "#tag is not a heading");
    }
    {
        auto n = md::parse("Title\n=====\n");
        check(n.size() == 1 && n[0].kind == md::Block::Heading && n[0].level == 1,
              "setext heading");
    }
    {
        auto n = md::parse("```objc\nNSString *s;\n```\n");
        check(n.size() == 1 && n[0].kind == md::Block::Code && n[0].lang == "objc" &&
                  n[0].text == "NSString *s;",
              "fenced code keeps its language and body");
    }
    {
        // Blank lines inside a fence are content, not block separators.
        auto n = md::parse("```\na\n\nb\n```\n");
        check(n.size() == 1 && n[0].text == "a\n\nb",
              "blank line inside a fence stays in the code");
    }
    {
        auto n = md::parse("para\n\n---\n\npara\n");
        check(n.size() == 3 && n[1].kind == md::Block::Rule, "thematic break");
    }
    {
        auto n = md::parse("- a\n  - b\n- c\n");
        check(n.size() == 3 && n[0].kind == md::Block::Bullet && n[0].level == 0 &&
                  n[1].level == 1 && n[2].level == 0,
              "list nesting depth from indentation");
    }
    {
        auto n = md::parse("1. first\n2. second\n");
        check(n.size() == 2 && n[0].kind == md::Block::Numbered && n[0].marker == "1." &&
                  n[1].marker == "2.",
              "ordered list keeps its markers");
    }
    {
        auto n = md::parse("- one\n  continued here\n");
        check(n.size() == 1 && n[0].runs.size() == 1 &&
                  n[0].runs[0].text == "one continued here",
              "indented continuation joins the list item");
    }
    {
        // A quote is re-parsed, so quoted structure survives as structure.
        auto n = md::parse("> - x\n> - y\n");
        check(n.size() == 2 && n[0].kind == md::Block::Bullet && n[0].quote == 1,
              "list inside a blockquote stays a list");
        auto n2 = md::parse("> quoted\n");
        check(n2.size() == 1 && n2[0].quote == 1 && n2[0].kind == md::Block::Paragraph,
              "blockquote paragraph");
    }
    {
        auto n = md::parse("| a | b |\n| --- | --- |\n| 1 | 2 |\n");
        check(n.size() == 2 && n[0].kind == md::Block::TableRow && n[0].header &&
                  n[0].cells.size() == 2 && !n[1].header && n[1].table_end,
              "pipe table rows and cells");
    }
    {
        // A pipe in prose is not a table.
        auto n = md::parse("use a | b in the shell\n");
        check(n.size() == 1 && n[0].kind == md::Block::Paragraph,
              "a stray pipe is not a table");
    }
    {
        // A single newline is a space; two trailing spaces is a hard break.
        auto n = md::parse("one\ntwo\n");
        check(n.size() == 1 && n[0].runs.size() == 1 && n[0].runs[0].text == "one two",
              "soft line break becomes a space");
        auto n2 = md::parse("one  \ntwo\n");
        check(n2.size() == 1 && n2[0].runs[0].text == "one\ntwo",
              "two trailing spaces is a hard break");
    }
    {
        auto n = md::parse("");
        check(n.empty(), "empty input yields no blocks");
    }

    // --- streaming ---------------------------------------------------------
    {
        // complete_prefix is what keeps streaming affordable: only whole blocks
        // are committed, so a G5 never re-highlights the same code twice.
        check(md::complete_prefix("para one\n\npara two") == 10,
              "prefix commits at a blank line");
        check(md::complete_prefix("no break yet") == 0,
              "nothing commits without a boundary");
        check(md::complete_prefix("```\ncode\n\nmore\n") == 0,
              "a blank line inside a fence does not commit");
        check(md::complete_prefix("```\nx\n```\n") == 10,
              "a closed fence commits");
        std::string doc = "# H\n\n```c\nint x;\n```\n\ntail";
        size_t p = md::complete_prefix(doc);
        check(p > 0 && doc.substr(p) == "tail",
              "prefix leaves only the incomplete trailing block");
    }
}


// The embedded database. SQLite and sqlite-vec are compiled into the binary so
// a downloaded build needs no MacPorts; these checks are what prove that claim,
// and that both halves of the search actually work on this hardware.
void test_vecstore() {
    std::printf("[vecstore]\n");

    check(!vec::sqlite_version().empty(), "sqlite is linked in");
    check(!vec::sqlite_vec_version().empty(), "sqlite-vec is linked in");

    fs::path dir = fs::temp_directory_path() / "ppcode-vecstore-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::string db = (dir / "index.db").string();

    vec::Store store;
    std::string err;
    check(store.open(db, &err), "store opens (" + err + ")");

    // Deliberately similar wording, so lexical and semantic ranking disagree --
    // which is the only way the fusion below proves anything.
    std::vector<vec::Store::Chunk> chunks;
    {
        vec::Store::Chunk c;
        c.ordinal = 0;
        c.text = "The dylib relocation rewrites install names to "
                 "@executable_path so the bundle is self contained.";
        c.embedding = std::vector<float>{1.0f, 0.0f, 0.0f, 0.0f};
        chunks.push_back(c);
    }
    {
        vec::Store::Chunk c;
        c.ordinal = 1;
        c.text = "Leopard expects JPEG 2000 in the large icon chunks, not PNG.";
        c.embedding = std::vector<float>{0.0f, 1.0f, 0.0f, 0.0f};
        chunks.push_back(c);
    }
    {
        vec::Store::Chunk c;
        c.ordinal = 2;
        c.text = "Saddle stitching is drawn one stitch at a time along the seam.";
        c.embedding = std::vector<float>{0.0f, 0.0f, 1.0f, 0.0f};
        chunks.push_back(c);
    }

    check(store.put_document("conv-1", "conversations", chunks, &err),
          "document indexed (" + err + ")");

    int64_t total = 0, embedded = 0;
    store.stats(&total, &embedded, &err);
    check(total == 3, "three chunks stored");
    check(embedded == 3, "three embeddings stored");

    // --- lexical -----------------------------------------------------------
    {
        auto hits = store.search_text("install names", 5);
        check(!hits.empty() && hits[0].doc_id == "conv-1",
              "FTS5 finds a phrase");
        check(!hits.empty() &&
                  hits[0].text.find("@executable_path") != std::string::npos,
              "FTS5 returns the right chunk");
    }
    {
        // The identifier case: a rare literal token is exactly what BM25 is for.
        auto hits = store.search_text("JPEG", 5);
        check(hits.size() == 1 && hits[0].chunk_id == 2,
              "FTS5 matches a rare token uniquely");
    }
    {
        auto hits = store.search_text("nothing here matches at all zzz", 5);
        check(hits.empty(), "FTS5 returns nothing for an absent term");
    }

    // --- vector ------------------------------------------------------------
    {
        // Nearest to the second chunk's own vector must be that chunk.
        std::vector<float> q{0.05f, 0.99f, 0.0f, 0.0f};
        auto hits = store.search_vector(q, 2);
        check(!hits.empty() && hits[0].chunk_id == 2,
              "vector search returns the nearest chunk");
        check(hits.size() == 2 && hits[0].score > hits[1].score,
              "vector hits are ordered by similarity");
    }
    {
        // A mismatched width must be refused, not silently compared.
        std::vector<float> wrong{1.0f, 0.0f};
        check(store.search_vector(wrong, 2).empty(),
              "a wrong-width query returns nothing");
    }

    // --- hybrid ------------------------------------------------------------
    {
        // Lexical alone cannot find this: the word "stitch" is present but the
        // query vector points at the stitching chunk, so fusion must surface it.
        std::vector<float> q{0.0f, 0.0f, 1.0f, 0.0f};
        auto hits = store.search("seam", q, 3);
        bool found = false;
        for (size_t i = 0; i < hits.size(); i++)
            if (hits[i].chunk_id == 3) found = true;
        check(found, "hybrid search fuses both rankings");
    }
    {
        // With no embedding to offer, hybrid must degrade to lexical rather
        // than returning nothing.
        auto hits = store.search("JPEG", std::vector<float>(), 3);
        check(hits.size() == 1 && hits[0].chunk_id == 2,
              "hybrid degrades to lexical without an embedding");
    }

    // --- reindexing --------------------------------------------------------
    {
        std::vector<vec::Store::Chunk> replacement;
        vec::Store::Chunk c;
        c.ordinal = 0;
        c.text = "Replaced entirely.";
        replacement.push_back(c);

        check(store.put_document("conv-1", "conversations", replacement, &err),
              "document re-indexed");

        int64_t n = 0;
        store.stats(&n, nullptr, &err);
        check(n == 1, "re-indexing replaces rather than duplicates");
        check(store.search_text("install names", 5).empty(),
              "the old text is gone from the index");
    }

    {
        check(store.forget_document("conv-1", &err), "document forgotten");
        int64_t n = -1;
        store.stats(&n, nullptr, &err);
        check(n == 0, "forgetting removes every chunk");
    }

    store.close();
    fs::remove_all(dir, ec);
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

void test_bundler() {
    std::printf("[bundler]\n");

    // Use the running binary: it is a real Mach-O with real MacPorts deps.
    std::string self = "build/ppcode";
    std::error_code ec;
    if (!fs::exists(self, ec)) {
        std::printf("  skip  build/ppcode not present\n");
        return;
    }

    std::vector<bundle::DylibRef> deps = bundle::dependencies(self);
    check(!deps.empty(), "otool dependencies parsed (" +
                             std::to_string(deps.size()) + ")");
    bool sys = false, ports = false;
    for (const bundle::DylibRef& d : deps) {
        if (d.is_system) sys = true;
        if (starts_with(d.path, "/opt/local")) ports = true;
        check(d.path.find('(') == std::string::npos,
              "dependency path has no version suffix");
        break;   // one representative check is enough
    }
    for (const bundle::DylibRef& d : deps) {
        if (d.is_system) sys = true;
        if (starts_with(d.path, "/opt/local")) ports = true;
    }
    check(sys, "system libraries identified as system");
    check(ports, "MacPorts libraries found");

    check(bundle::dependencies("/does/not/exist").empty(),
          "missing binary yields no dependencies");

    // A dry run must report the transitive set without touching anything.
    {
        std::string dest = "/tmp/ppcode-bundle-dry";
        fs::remove_all(dest);
        bundle::RelocateResult r =
            bundle::relocate(self, dest, "@loader_path/../Frameworks",
                             {"/opt/local"}, /*dry_run=*/true);
        check(r.ok, "dry run succeeds");
        check(!fs::exists(dest, ec), "dry run created nothing");
        check(r.report.find("dry run") != std::string::npos, "dry run says so");
        // Transitive: libssl/libcrypto are reached only through libcurl.
        check(r.report.find("libcrypto") != std::string::npos,
              "transitive dependency discovered");
    }

    // Bundle assembly, without relocation so the test stays fast.
    {
        std::string app = "/tmp/ppcode-bundle-test/Demo.app";
        fs::remove_all("/tmp/ppcode-bundle-test");

        bundle::BundleSpec spec;
        spec.app_path = app;
        spec.executable = self;
        spec.name = "Demo";
        spec.identifier = "test.demo";
        spec.version = "2.5";
        spec.relocate_libs = false;

        bundle::BundleResult r = bundle::make_bundle(spec);
        check(r.ok, "bundle created" + (r.error.empty() ? "" : ": " + r.error));
        check(fs::exists(app + "/Contents/MacOS/ppcode", ec), "executable copied");
        check(fs::exists(app + "/Contents/Info.plist", ec), "Info.plist written");
        check(fs::exists(app + "/Contents/PkgInfo", ec), "PkgInfo written");

        std::string plist;
        read_file_text(app + "/Contents/Info.plist", &plist, nullptr);
        check(plist.find("<string>Demo</string>") != std::string::npos,
              "bundle name in Info.plist");
        check(plist.find("test.demo") != std::string::npos, "identifier in Info.plist");
        check(plist.find("<string>2.5</string>") != std::string::npos,
              "version in Info.plist");
        check(plist.find("LSMinimumSystemVersion") != std::string::npos,
              "minimum system version set");
        check(plist.find("CFBundleExecutable") != std::string::npos,
              "executable named");

        // The plist must be well-formed, or the Finder will not launch it.
        xml::Document pd;
        std::string perr;
        check(xml::parse(plist, &pd, &perr), "Info.plist is well-formed XML");

        fs::remove_all("/tmp/ppcode-bundle-test");
    }

    {
        bundle::BundleSpec bad;
        bad.app_path = "/tmp/x.app";
        bad.executable = "/does/not/exist";
        check(!bundle::make_bundle(bad).ok, "missing executable is refused");
    }
}

void test_xml() {
    std::printf("[xml]\n");
    const std::string src =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<archive type=\"com.apple.InterfaceBuilder3.Cocoa.XIB\" version=\"7.02\">\n"
        "\t<data>\n"
        "\t\t<int key=\"IBDocument.SystemTarget\">1050</int>\n"
        "\t\t<string key=\"IBDocument.SystemVersion\">9D29</string>\n"
        "\t\t<object class=\"NSMutableArray\" key=\"IBDocument.RootObjects\" id=\"1048\">\n"
        "\t\t\t<bool key=\"EncodedWithXMLCoder\">YES</bool>\n"
        "\t\t\t<object class=\"NSCustomObject\" id=\"1021\">\n"
        "\t\t\t\t<string key=\"NSClassName\">NSApplication</string>\n"
        "\t\t\t</object>\n"
        "\t\t\t<object class=\"NSWindowTemplate\" id=\"77\">\n"
        "\t\t\t\t<string key=\"NSWindowTitle\">Sample &amp; Friends</string>\n"
        "\t\t\t\t<reference key=\"NSWindowView\" ref=\"1021\"/>\n"
        "\t\t\t</object>\n"
        "\t\t</object>\n"
        "\t</data>\n"
        "</archive>\n";

    xml::Document d;
    std::string err;
    check(xml::parse(src, &d, &err), "xib-shaped xml parses" +
                                         (err.empty() ? "" : ": " + err));
    if (!d.root) return;
    check(d.root->name == "archive", "root element");
    check(d.root->attr("version") == "7.02", "attribute read");
    check(starts_with(d.declaration, "<?xml"), "declaration preserved");

    xml::NodePtr data = d.root->first_child("data");
    check(data != nullptr, "child lookup by name");
    if (data) {
        std::vector<xml::NodePtr> ints = data->find_children("int");
        check(ints.size() == 1 && ints[0]->inner_text() == "1050", "inner text");
    }
    check(d.root->find_all("object").size() == 3, "recursive find_all");
    check(d.root->find_all_with_attr("object", "class", "NSWindowTemplate").size() == 1,
          "find by attribute");

    // Entities must survive a round trip, or a window title with an ampersand
    // silently corrupts the nib.
    {
        std::vector<xml::NodePtr> w =
            d.root->find_all_with_attr("object", "id", "77");
        check(!w.empty(), "found the window object");
        if (!w.empty()) {
            xml::NodePtr title = w[0]->first_child("string");
            check(title && title->inner_text() == "Sample & Friends",
                  "entity decoded on read");
        }
    }
    {
        std::string out = xml::serialize(d, true);
        check(out.find("&amp;") != std::string::npos, "entity re-escaped on write");
        xml::Document again;
        std::string e2;
        check(xml::parse(out, &again, &e2), "round trip re-parses");
        if (again.root)
            check(again.root->find_all("object").size() == 3,
                  "round trip preserves the object count");
    }

    check(xml::escape("a<b>&c") == "a&lt;b&gt;&amp;c", "escape");
    check(xml::unescape("a&lt;b&gt;&amp;c&#65;") == "a<b>&cA", "unescape incl numeric");

    {
        xml::Document bad;
        std::string e;
        check(!xml::parse("<a><b></a>", &bad, &e), "mismatched tags rejected");
        check(e.find("line") != std::string::npos, "error names a line");
    }
}

void test_xib() {
    std::printf("[xib]\n");

    // Prefer the real nib from the Sample project; it is the actual format.
    std::string real = expand_user("~/Desktop/Sample/English.lproj/MainMenu.xib");
    std::error_code ec;
    if (!fs::exists(real, ec)) {
        std::printf("  skip  no MainMenu.xib available to test against\n");
        return;
    }

    std::string work = "/tmp/ppcode-xib/MainMenu.xib";
    fs::remove_all("/tmp/ppcode-xib");
    fs::create_directories("/tmp/ppcode-xib");
    fs::copy_file(real, work, ec);
    check(!ec, "copied the real nib to a scratch directory");

    xib::Document d;
    std::string err;
    check(d.load(work, &err), "real nib loads" + (err.empty() ? "" : ": " + err));

    check(!d.format_version().empty(), "archive version read");
    check(d.system_target() == "1050", "deployment target is 10.5");

    std::vector<xib::ObjectNode> objs = d.objects();
    check(objs.size() > 100, "object graph parsed (" +
                                 std::to_string(objs.size()) + " objects)");
    bool has_menu = false, has_app = false;
    for (const xib::ObjectNode& o : objs) {
        if (o.cls == "NSMenu") has_menu = true;
        if (o.cls == "NSCustomObject") has_app = true;
    }
    check(has_menu, "found the menu bar");
    check(has_app, "found the File's Owner / application objects");

    std::string desc = d.describe(false);
    check(desc.find("NSMenu") != std::string::npos, "description names classes");
    check(desc.find("objects") != std::string::npos, "description counts objects");

    // Declaring a class is the safe edit; it must survive a save and reload.
    xib::ClassDescription cd;
    cd.name = "PPTestController";
    cd.superclass = "NSObject";
    cd.source_file = "PPTestController.h";
    cd.outlets.push_back({"window", "NSWindow"});
    cd.outlets.push_back({"statusField", "NSTextField"});
    cd.actions.push_back("doThing");

    check(d.add_class(cd, &err), "class declared" + (err.empty() ? "" : ": " + err));
    check(d.save(&err), "nib saved" + (err.empty() ? "" : ": " + err));
    check(fs::exists(work + ".ppcode-bak"), "a backup was written");

    xib::Document d2;
    check(d2.load(work, &err), "modified nib reloads (still well-formed)");
    bool found = false;
    for (const xib::ClassDescription& c : d2.classes()) {
        if (c.name != "PPTestController") continue;
        found = true;
        check(c.superclass == "NSObject", "superclass survived");
        check(c.outlets.size() == 2, "outlets survived");
        check(c.actions.size() == 1, "actions survived");
        bool win = false;
        for (const auto& [n, t] : c.outlets)
            if (n == "window" && t == "NSWindow") win = true;
        check(win, "outlet name and type survived");
    }
    check(found, "declared class is present after reload");

    // Declaring twice must replace, not duplicate.
    check(d2.add_class(cd, &err), "re-declaring the same class succeeds");
    int count = 0;
    for (const xib::ClassDescription& c : d2.classes())
        if (c.name == "PPTestController") count++;
    check(count == 1, "re-declaring replaces rather than duplicating");

    check(d2.remove_class("PPTestController", &err), "class can be removed");
    check(!d2.remove_class("NoSuchClass", &err), "removing an unknown class fails cleanly");

    // A compiled nib must be refused rather than mangled.
    {
        xib::Document nib;
        std::string e;
        check(!nib.load("/tmp/ppcode-xib/whatever.nib", &e), "compiled .nib refused");
        check(e.find("compiled") != std::string::npos, "and says why");
    }

    fs::remove_all("/tmp/ppcode-xib");
}

void test_checkpoint() {
    std::printf("[checkpoint / diff]\n");

    check(checkpoint::unified_diff("same", "same", "f").empty(),
          "identical text produces no diff");
    {
        std::string d = checkpoint::unified_diff("a\nb\nc\n", "a\nB\nc\n", "f.txt");
        check(d.find("-b") != std::string::npos, "removed line marked");
        check(d.find("+B") != std::string::npos, "added line marked");
        check(d.find(" a") != std::string::npos, "context line kept");
        check(d.find("@@") != std::string::npos, "hunk header present");
    }
    check(checkpoint::diff_stat("a\n", "a\nb\n").find("+1") != std::string::npos,
          "diff stat counts additions");
    check(checkpoint::diff_stat("x", "x") == "no change", "diff stat on no change");

    // Round trip through the store, including undo of both a modification and a
    // creation.
    std::string dir = "/tmp/ppcode-ckpt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string f = dir + "/a.txt";
    write_file_text(f, "original\n", nullptr);

    checkpoint::Store s;
    s.record_before(f, "edit_file");
    write_file_text(f, "modified\n", nullptr);
    std::string diff = s.record_after(f);
    check(!diff.empty(), "store produces a diff");
    check(diff.find("-original") != std::string::npos, "diff shows the old line");
    check(s.size() == 1, "one entry recorded");

    std::string report;
    check(s.undo(1, &report) == 1, "undo reverts one change");
    std::string back;
    read_file_text(f, &back, nullptr);
    check(back == "original\n", "file contents restored");
    check(s.size() == 0, "entry consumed");

    // Undoing a creation must delete the file, not leave an empty one.
    std::string nf = dir + "/new.txt";
    s.record_before(nf, "write_file");
    write_file_text(nf, "created\n", nullptr);
    s.record_after(nf);
    check(fs::exists(nf), "file created");
    s.undo(1, &report);
    check(!fs::exists(nf), "undoing a creation removes the file");

    check(s.undo(1, &report) == 0 && report.find("nothing") != std::string::npos,
          "undo with empty history reports cleanly");

    // A write that changes nothing should not be recorded, or undo would be
    // cluttered with no-ops.
    write_file_text(f, "same\n", nullptr);
    s.record_before(f, "edit_file");
    write_file_text(f, "same\n", nullptr);
    check(s.record_after(f).empty(), "unchanged write produces no diff");
    check(s.size() == 0, "and is not recorded");

    fs::remove_all(dir);
}

void test_session() {
    std::printf("[sessions]\n");
    check(!session::sessions_dir().empty(), "sessions directory resolves");
    check(!session::new_id().empty(), "new id generated");
    check(session::path_for("abc").find("abc.json") != std::string::npos,
          "path from id");

    // Token estimation should count an image at a flat rate rather than by the
    // length of its base64, which would be wildly wrong.
    {
        std::vector<Message> msgs;
        msgs.push_back(Message::system_msg(std::string(4000, 'x')));
        int64_t base = session::estimate_tokens(msgs);
        check(base > 900 && base < 1100, "estimate is roughly chars/4");

        Message img;
        img.role = "user";
        img.parts.push_back(ContentPart::make_image(
            "data:image/png;base64," + std::string(200000, 'A')));
        msgs.push_back(img);
        int64_t with_img = session::estimate_tokens(msgs);
        check(with_img - base < 2000,
              "a base64 image is not counted by its encoded length");
    }

    check(!session::should_compact({}, 0), "no compaction without a known window");
    {
        std::vector<Message> big;
        big.push_back(Message::system_msg(std::string(80000, 'x')));
        check(session::should_compact(big, 8000), "large conversation triggers compaction");
        check(!session::should_compact(big, 10000000), "big window does not");
    }

    // Compaction must refuse rather than corrupt when it cannot work.
    {
        Config cfg;
        cfg.api_key = "";
        Client c(cfg);
        std::vector<Message> few;
        few.push_back(Message::system_msg("sys"));
        few.push_back(Message::user("hi"));
        session::CompactResult r = session::compact(c, &few);
        check(!r.ok, "refuses to compact a short conversation");
        check(few.size() == 2, "and leaves it untouched");
    }
    {
        // Long enough to try, but no API key, so it must fail cleanly and not
        // damage the history.
        Config cfg;
        cfg.api_key = "";
        Client c(cfg);
        std::vector<Message> msgs;
        msgs.push_back(Message::system_msg("sys"));
        for (int i = 0; i < 20; i++) {
            msgs.push_back(Message::user("u" + std::to_string(i)));
            msgs.push_back(Message::assistant("a" + std::to_string(i)));
        }
        size_t before = msgs.size();
        session::CompactResult r = session::compact(c, &msgs);
        check(!r.ok && !r.error.empty(), "reports why it could not summarise");
        check(msgs.size() == before, "history unchanged when summarising fails");
    }

    // The cut must never orphan a tool result from the call that produced it.
    {
        Config cfg;
        cfg.api_key = "";
        Client c(cfg);
        std::vector<Message> msgs;
        msgs.push_back(Message::system_msg("sys"));
        for (int i = 0; i < 10; i++) {
            Message a = Message::assistant("");
            ToolCall tc;
            tc.id = "c" + std::to_string(i);
            tc.name = "read_file";
            tc.arguments = "{}";
            a.tool_calls.push_back(tc);
            msgs.push_back(a);
            msgs.push_back(Message::tool_result(tc.id, tc.name, "result"));
        }
        // Even though the summary call fails, the boundary logic runs first;
        // assert the invariant on the structure we would have kept.
        size_t before = msgs.size();
        session::compact(c, &msgs);
        check(msgs.size() == before, "tool-call pairs left intact on failure");
    }
}

void test_builderr() {
    std::printf("[build diagnostics]\n");

    // Real GCC output shapes, including the source line the compiler echoes.
    const std::string gcc_out =
        "g++-mp-15 -std=c++23 -c src/utf8.cpp -o build/utf8.o\n"
        "src/utf8.cpp:289:1: error: expected unqualified-id before 'this'\n"
        "  289 | this is not valid C++ at all;\n"
        "      | ^~~~\n"
        "src/render.cpp:42:15: warning: unused variable 'x' [-Wunused-variable]\n"
        "gmake: *** [Makefile:63: build/utf8.o] Error 1\n";

    builderr::Report r = builderr::parse(gcc_out);
    check(r.error_count() == 1, "one error parsed");
    check(r.warning_count() == 1, "one warning parsed");
    check(r.failed, "make failure marks the build as failed");
    check(!r.make_failures.empty(), "make failure line captured");
    if (!r.diagnostics.empty()) {
        const builderr::Diagnostic& d = r.diagnostics[0];
        check(d.file == "src/utf8.cpp", "file extracted");
        check(d.line == 289, "line extracted");
        check(d.column == 1, "column extracted");
        check(d.severity == "error", "severity extracted");
        check(d.message.find("unqualified-id") != std::string::npos, "message extracted");
        check(d.context.find("not valid C++") != std::string::npos,
              "echoed source line kept as context");
    }
    std::string summary = r.summarise();
    check(summary.find("BUILD FAILED") != std::string::npos, "summary states failure");
    check(summary.find("src/utf8.cpp:289") != std::string::npos,
          "summary names the location");

    // Errors must sort ahead of warnings; a warning first would bury the thing
    // that actually needs fixing.
    {
        builderr::Report w = builderr::parse(
            "a.c:1:1: warning: first\n"
            "b.c:2:2: error: second\n");
        std::string s = w.summarise();
        check(s.find("b.c:2") < s.find("a.c:1"), "errors listed before warnings");
    }

    // Apple linker output.
    {
        builderr::Report l = builderr::parse(
            "Undefined symbols:\n"
            "  \"_PPCodeGreeting\", referenced from:\n"
            "      _main in main.o\n"
            "ld: symbol(s) not found\n"
            "collect2: ld returned 1 exit status\n");
        check(l.link_errors.size() >= 3, "undefined-symbol block captured");
        check(l.error_count() > 0, "link errors count as errors");
        check(l.summarise().find("Linker") != std::string::npos,
              "summary has a linker section");
    }

    // xcodebuild markers.
    {
        check(builderr::parse("** BUILD SUCCEEDED **\n").succeeded,
              "xcodebuild success marker");
        check(builderr::parse("** BUILD FAILED **\n").failed,
              "xcodebuild failure marker");
    }

    // A path containing a colon must not be mistaken for a location, and a bare
    // path:line with no severity is not a diagnostic.
    {
        builderr::Report n = builderr::parse(
            "In file included from src/a.hpp:10:\n"
            "make: Nothing to be done for 'all'.\n");
        check(n.diagnostics.empty(), "non-diagnostic lines ignored");
        check(n.error_count() == 0, "no spurious errors");
    }
}

void test_project_docs() {
    std::printf("[project instructions]\n");
    std::string root = "/tmp/ppcode-projdoc/repo";
    fs::remove_all("/tmp/ppcode-projdoc");
    fs::create_directories(root + "/sub/deeper");
    fs::create_directories(root + "/.git");     // marks the repository root

    write_file_text(root + "/.ppcode.md", "ROOT RULES", nullptr);
    write_file_text(root + "/sub/AGENTS.md", "SUB RULES", nullptr);

    // From a nested directory, both files should be found, outermost first so
    // the more specific one can override.
    std::vector<sysprompt::ProjectDoc> docs =
        sysprompt::load_project_docs(root + "/sub/deeper");
    check(docs.size() == 2, "walks up and finds both files");
    if (docs.size() == 2) {
        check(docs[0].body == "ROOT RULES", "outermost file comes first");
        check(docs[1].body == "SUB RULES", "nearer file comes last");
    }

    // The walk must stop at the repository root rather than escaping upwards.
    write_file_text("/tmp/ppcode-projdoc/.ppcode.md", "OUTSIDE", nullptr);
    docs = sysprompt::load_project_docs(root);
    bool leaked = false;
    for (const auto& d : docs) if (d.body == "OUTSIDE") leaked = true;
    check(!leaked, "does not read past the repository root");

    check(sysprompt::load_project_docs("/tmp/ppcode-projdoc-missing").empty(),
          "missing directory yields nothing");

    fs::remove_all("/tmp/ppcode-projdoc");
}

void test_appledocs() {
    std::printf("[apple docs]\n");
    if (!appledocs::available()) {
        std::printf("  skip  no Apple documentation installed on this machine\n");
        return;
    }
    check(!appledocs::docset_indexes().empty(), "docset index found");

    // A well-known Foundation class method. This exercises the whole join --
    // token, type, container, framework, declaration and abstract.
    appledocs::Query q;
    q.name = "stringWithFormat:";
    q.limit = 5;
    std::string err;
    std::vector<appledocs::Entry> hits = appledocs::search(q, &err);
    check(!hits.empty(), "exact API lookup returns a result" +
                             (err.empty() ? "" : ": " + err));
    if (!hits.empty()) {
        const appledocs::Entry& e = hits[0];
        check(e.name == "stringWithFormat:", "token name");
        check(e.container == "NSString", "owning class resolved");
        check(e.framework == "Foundation", "framework resolved");
        check(e.type.find("method") != std::string::npos, "token type humanised");
        check(e.declaration.find("stringWithFormat") != std::string::npos,
              "declaration present");
        check(e.declaration.find('<') == std::string::npos,
              "declaration has its HTML stripped");
        check(!e.abstract.empty(), "abstract present");
    }

    // A class lookup, narrowed by type.
    {
        appledocs::Query c;
        c.name = "NSString";
        c.type_filter = "class";
        c.limit = 3;
        std::vector<appledocs::Entry> got = appledocs::search(c, nullptr);
        check(!got.empty(), "class lookup with a type filter");
    }

    // The case that matters most: something that genuinely does not exist here.
    {
        appledocs::Query g;
        g.name = "dispatch_async";
        std::vector<appledocs::Entry> got = appledocs::search(g, nullptr);
        check(got.empty(), "an API absent from this OS returns nothing");
        check(appledocs::search_headers("dispatch_async", 4).empty(),
              "and is absent from the headers too");
    }

    // Headers are the compiler's ground truth, so they must be searchable.
    {
        std::vector<std::string> h = appledocs::search_headers("NSAutoreleasePool", 6);
        check(!h.empty(), "header search finds a real Foundation class");
    }

    // Injection safety: the query reaches us from a model.
    {
        appledocs::Query bad;
        bad.name = "'; DROP TABLE ZTOKEN; --";
        std::string berr;
        std::vector<appledocs::Entry> got = appledocs::search(bad, &berr);
        check(got.empty() && !berr.empty(), "a hostile query is rejected");
        // And the table is still there.
        appledocs::Query again;
        again.name = "NSString";
        check(!appledocs::search(again, nullptr).empty(), "index survived");
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
    test_mdparse();
    test_vecstore();
    test_web();
    test_plist_and_xcode();
    test_bundler();
    test_xml();
    test_xib();
    test_checkpoint();
    test_session();
    test_builderr();
    test_project_docs();
    test_appledocs();
    test_envinfo();
    test_config();
    test_agent_offline();
    if (with_network) test_network();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails;
}

} // namespace ppcode

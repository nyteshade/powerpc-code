// selftest.cpp -- offline checks for the parts that are easy to get subtly
// wrong (SSE framing, streaming tool-call assembly, the edit tool's uniqueness
// rule), plus an optional live network probe.
#include "selftest.hpp"

#include "agent.hpp"
#include "common.hpp"
#include "config.hpp"
#include "http.hpp"
#include "openrouter.hpp"
#include "tools.hpp"

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
    test_sse_parser();
    test_stream_assembler();
    test_message_serialisation();
    test_glob_and_shell();
    test_tools();
    test_config();
    test_agent_offline();
    if (with_network) test_network();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails;
}

} // namespace ppcode

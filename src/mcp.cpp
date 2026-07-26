#include "mcp.hpp"

#include "http.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ppcode::mcp {

namespace {

int64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

// ---------------------------------------------------------------------------
// stdio transport
// ---------------------------------------------------------------------------

class StdioTransport : public Transport {
public:
    explicit StdioTransport(McpServerConfig cfg) : cfg_(std::move(cfg)) {}
    ~StdioTransport() override { stop(); }

    bool start(std::string* error) override {
        int to_child[2], from_child[2], err_pipe[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0 || pipe(err_pipe) != 0) {
            if (error) *error = std::string("pipe: ") + std::strerror(errno);
            return false;
        }

        pid_ = fork();
        if (pid_ < 0) {
            if (error) *error = std::string("fork: ") + std::strerror(errno);
            return false;
        }

        if (pid_ == 0) {
            setpgid(0, 0);
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            close(to_child[0]);  close(to_child[1]);
            close(from_child[0]); close(from_child[1]);
            close(err_pipe[0]);   close(err_pipe[1]);

            for (const auto& [k, v] : cfg_.env) setenv(k.c_str(), v.c_str(), 1);

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(cfg_.command.c_str()));
            for (const std::string& a : cfg_.args)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            execvp(cfg_.command.c_str(), argv.data());
            std::fprintf(stderr, "exec %s failed: %s\n", cfg_.command.c_str(),
                         std::strerror(errno));
            _exit(127);
        }

        setpgid(pid_, pid_);
        close(to_child[0]);
        close(from_child[1]);
        close(err_pipe[1]);
        in_fd_ = to_child[1];
        out_fd_ = from_child[0];
        err_fd_ = err_pipe[0];

        // Non-blocking stderr so drain_stderr never wedges.
        fcntl(err_fd_, F_SETFL, O_NONBLOCK);
        return true;
    }

    bool send(const json& msg, std::string* error) override {
        if (in_fd_ < 0) { if (error) *error = "not connected"; return false; }
        // MCP stdio framing: one JSON object per line, no embedded newlines.
        std::string line = msg.dump();
        line += "\n";
        size_t off = 0;
        while (off < line.size()) {
            ssize_t n = write(in_fd_, line.data() + off, line.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (error) *error = std::string("write: ") + std::strerror(errno);
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool receive(json* out, int timeout_ms, std::string* error) override {
        int64_t deadline = now_ms() + timeout_ms;
        for (;;) {
            // A complete line may already be buffered from a previous read.
            size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                if (trim(line).empty()) continue;
                try {
                    *out = json::parse(line);
                    return true;
                } catch (const std::exception& e) {
                    log_line("mcp: bad json from server: " + std::string(e.what()));
                    continue;   // skip the junk line and keep looking
                }
            }

            int64_t remaining = deadline - now_ms();
            if (remaining <= 0) { if (error) *error = "timeout"; return false; }

            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(out_fd_, &rd);
            struct timeval tv;
            int64_t slice = std::min<int64_t>(remaining, 200);
            tv.tv_sec = static_cast<long>(slice / 1000);
            tv.tv_usec = static_cast<long>((slice % 1000) * 1000);

            int n = select(out_fd_ + 1, &rd, nullptr, nullptr, &tv);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (error) *error = std::string("select: ") + std::strerror(errno);
                return false;
            }
            if (n == 0) continue;

            char tmp[8192];
            ssize_t got = read(out_fd_, tmp, sizeof(tmp));
            if (got < 0) {
                if (errno == EINTR) continue;
                if (error) *error = std::string("read: ") + std::strerror(errno);
                return false;
            }
            if (got == 0) {
                if (error) *error = "server closed the connection";
                dead_ = true;
                return false;
            }
            buf_.append(tmp, static_cast<size_t>(got));
        }
    }

    void stop() override {
        if (in_fd_ >= 0)  { close(in_fd_);  in_fd_ = -1; }
        if (pid_ > 0) {
            // Closing stdin asks the server to exit; give it a moment.
            for (int i = 0; i < 20; i++) {
                int st;
                pid_t r = waitpid(pid_, &st, WNOHANG);
                if (r == pid_) { pid_ = -1; break; }
                usleep(50000);
            }
            if (pid_ > 0) {
                kill(-pid_, SIGTERM);
                usleep(100000);
                int st;
                if (waitpid(pid_, &st, WNOHANG) != pid_) {
                    kill(-pid_, SIGKILL);
                    waitpid(pid_, &st, 0);
                }
                pid_ = -1;
            }
        }
        if (out_fd_ >= 0) { close(out_fd_); out_fd_ = -1; }
        if (err_fd_ >= 0) { close(err_fd_); err_fd_ = -1; }
    }

    bool alive() const override { return pid_ > 0 && !dead_; }

    std::string drain_stderr() override {
        if (err_fd_ < 0) return "";
        std::string out;
        char tmp[4096];
        for (;;) {
            ssize_t n = read(err_fd_, tmp, sizeof(tmp));
            if (n <= 0) break;
            out.append(tmp, static_cast<size_t>(n));
            if (out.size() > 8192) break;
        }
        return out;
    }

private:
    McpServerConfig cfg_;
    pid_t pid_ = -1;
    int in_fd_ = -1, out_fd_ = -1, err_fd_ = -1;
    std::string buf_;
    bool dead_ = false;
};

// ---------------------------------------------------------------------------
// Streamable HTTP transport
// ---------------------------------------------------------------------------

class HttpTransport : public Transport {
public:
    explicit HttpTransport(McpServerConfig cfg) : cfg_(std::move(cfg)) {}

    bool start(std::string*) override { return true; }

    bool send(const json& msg, std::string* error) override {
        pending_.clear();

        http::Headers h;
        h.push_back("Accept: application/json, text/event-stream");
        for (const auto& [k, v] : cfg_.headers) h.push_back(k + ": " + v);
        if (!session_id_.empty()) h.push_back("Mcp-Session-Id: " + session_id_);
        h.push_back(std::string("MCP-Protocol-Version: ") + kProtocolVersion);

        http::Response r = http::post_json(cfg_.url, h, msg.dump(), 120);
        if (!r.error.empty()) { if (error) *error = r.error; return false; }

        // A notification legitimately returns 202 with no body.
        if (r.status == 202 || trim(r.body).empty()) return true;

        if (r.status < 200 || r.status >= 300) {
            if (error)
                *error = "HTTP " + std::to_string(r.status) + ": " +
                         json_preview(r.body, 200);
            return false;
        }

        // The body is either a bare JSON-RPC object or an SSE stream carrying
        // one or more of them.
        std::string body = trim(r.body);
        if (!body.empty() && body[0] == '{') {
            try {
                pending_.push_back(json::parse(body));
                return true;
            } catch (const std::exception& e) {
                if (error) *error = e.what();
                return false;
            }
        }

        http::SseParser parser;
        auto handler = [&](const http::SseEvent& e) {
            std::string d = trim(e.data);
            if (d.empty() || d == "[DONE]") return true;
            try {
                pending_.push_back(json::parse(d));
            } catch (const std::exception&) {
                log_line("mcp/http: unparseable SSE payload");
            }
            return true;
        };
        parser.feed(body.data(), body.size(), handler);
        parser.finish(handler);
        return true;
    }

    bool receive(json* out, int, std::string* error) override {
        if (pending_.empty()) {
            if (error) *error = "no response";
            return false;
        }
        *out = pending_.front();
        pending_.pop_front();
        return true;
    }

    void stop() override {}
    bool alive() const override { return true; }

    void set_session(const std::string& s) { session_id_ = s; }

private:
    McpServerConfig cfg_;
    std::deque<json> pending_;
    std::string session_id_;
};

} // namespace

std::unique_ptr<Transport> make_stdio_transport(const McpServerConfig& cfg) {
    return std::make_unique<StdioTransport>(cfg);
}

std::unique_ptr<Transport> make_http_transport(const McpServerConfig& cfg) {
    return std::make_unique<HttpTransport>(cfg);
}

std::string qualified_tool_name(const std::string& server, const std::string& tool) {
    std::string s = server + "__" + tool;
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
            out += c;
        else
            out += '_';
    }
    // The API caps function names at 64 characters.
    if (out.size() > 64) out = out.substr(0, 64);
    return out;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

Server::~Server() { disconnect(); }

bool Server::request(const std::string& method, const json& params, json* result,
                     int timeout_ms, std::string* error) {
    if (!transport_) { if (error) *error = "not connected"; return false; }

    int id = next_id_++;
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) req["params"] = params;

    if (!transport_->send(req, error)) return false;

    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) { if (error) *error = "timed out waiting for " + method; return false; }

        json msg;
        if (!transport_->receive(&msg, static_cast<int>(remaining), error)) return false;

        // Skip notifications and responses to other requests.
        const json* mid = jptr(msg, "id");
        if (!mid) {
            log_line("mcp: notification " + jstr(msg, "method"));
            continue;
        }
        if (!mid->is_number() || mid->get<int>() != id) continue;

        if (const json* err = jptr(msg, "error")) {
            if (error)
                *error = jstr(*err, "message", err->dump()) +
                         " (code " + std::to_string(jint(*err, "code")) + ")";
            return false;
        }
        if (const json* res = jptr(msg, "result")) *result = *res;
        else *result = json::object();
        return true;
    }
}

bool Server::notify(const std::string& method, const json& params, std::string* error) {
    json n = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) n["params"] = params;
    return transport_->send(n, error);
}

bool Server::connect(std::string* error) {
    if (cfg_.transport == "http") transport_ = make_http_transport(cfg_);
    else transport_ = make_stdio_transport(cfg_);

    if (!transport_->start(error)) return false;

    json init_params = {
        {"protocolVersion", kProtocolVersion},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "ppcode"}, {"version", "0.1"}}},
    };

    json result;
    if (!request("initialize", init_params, &result, 30000, error)) {
        std::string se = transport_->drain_stderr();
        if (!se.empty() && error) *error += "\n  server stderr: " + json_preview(se, 400);
        transport_->stop();
        return false;
    }

    std::string ver = jstr(result, "protocolVersion", "?");
    std::string sname = "?", sver = "";
    if (const json* si = jptr(result, "serverInfo")) {
        sname = jstr(*si, "name", "?");
        sver = jstr(*si, "version", "");
    }
    server_info_ = sname + (sver.empty() ? "" : " " + sver) + " (protocol " + ver + ")";

    if (!notify("notifications/initialized", json::object(), error)) {
        transport_->stop();
        return false;
    }

    // Fetch the tool list, following pagination if the server uses it.
    tools_.clear();
    std::string cursor;
    for (int page = 0; page < 20; page++) {
        json params = json::object();
        if (!cursor.empty()) params["cursor"] = cursor;

        json list;
        if (!request("tools/list", params, &list, 30000, error)) {
            transport_->stop();
            return false;
        }
        const json* arr = jptr(list, "tools");
        if (arr && arr->is_array()) {
            for (const json& t : *arr) {
                RemoteTool rt;
                rt.name = jstr(t, "name");
                if (rt.name.empty()) continue;
                rt.description = jstr(t, "description");
                if (const json* s = jptr(t, "inputSchema")) rt.input_schema = *s;
                else rt.input_schema = json{{"type", "object"}, {"properties", json::object()}};
                tools_.push_back(std::move(rt));
            }
        }
        cursor = jstr(list, "nextCursor");
        if (cursor.empty()) break;
    }

    connected_ = true;
    return true;
}

void Server::disconnect() {
    if (transport_) {
        transport_->stop();
        transport_.reset();
    }
    connected_ = false;
}

ToolResult Server::call(const std::string& tool_name, const json& args, int timeout_ms) {
    if (!connected_ || !transport_)
        return ToolResult::err("MCP server '" + cfg_.name + "' is not connected");

    json params = {{"name", tool_name},
                   {"arguments", args.is_null() ? json::object() : args}};

    json result;
    std::string err;
    if (!request("tools/call", params, &result, timeout_ms, &err)) {
        std::string se = transport_->drain_stderr();
        if (!se.empty()) log_line("mcp stderr: " + se);
        return ToolResult::err(cfg_.name + ": " + err);
    }

    // Result content is a list of typed parts; we flatten text and describe
    // anything else rather than dropping it silently.
    std::string text;
    if (const json* content = jptr(result, "content"); content && content->is_array()) {
        for (const json& part : *content) {
            std::string type = jstr(part, "type");
            if (type == "text") {
                if (!text.empty()) text += "\n";
                text += jstr(part, "text");
            } else if (type == "resource") {
                if (const json* r = jptr(part, "resource")) {
                    if (!text.empty()) text += "\n";
                    std::string t = jstr(*r, "text");
                    text += t.empty() ? ("[resource " + jstr(*r, "uri") + "]") : t;
                }
            } else {
                if (!text.empty()) text += "\n";
                text += "[" + (type.empty() ? "unknown" : type) + " content]";
            }
        }
    }
    if (text.empty()) {
        // Some servers return structuredContent instead.
        if (const json* sc = jptr(result, "structuredContent")) text = sc->dump(2);
        else text = "(no content)";
    }

    bool is_err = jbool(result, "isError", false);
    return is_err ? ToolResult::err(text) : ToolResult::ok(text);
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

void Manager::connect_all(const std::vector<McpServerConfig>& configs,
                          ToolRegistry& registry,
                          const std::function<void(const std::string&)>& report) {
    for (const McpServerConfig& cfg : configs) {
        if (!cfg.enabled) continue;

        auto server = std::make_shared<Server>(cfg);
        std::string err;
        if (!server->connect(&err)) {
            // One bad server must not stop the others or the program.
            if (report) report("mcp: " + cfg.name + " failed: " + err);
            log_line("mcp connect failed for " + cfg.name + ": " + err);
            continue;
        }

        int added = 0;
        for (const RemoteTool& rt : server->tools()) {
            Tool t;
            t.spec.name = qualified_tool_name(cfg.name, rt.name);
            t.spec.description =
                rt.description.empty()
                    ? ("Tool " + rt.name + " from MCP server " + cfg.name)
                    : rt.description;
            t.spec.parameters = rt.input_schema;
            // We cannot know whether a remote tool mutates anything, so treat
            // them all as requiring approval.
            t.kind = ToolKind::Execute;
            t.source = "mcp:" + cfg.name;

            std::string remote_name = rt.name;
            std::string srv = cfg.name;
            t.handler = [server, remote_name](const json& args, ToolContext&) {
                return server->call(remote_name, args, 120000);
            };
            t.preview = [srv, remote_name](const json& a) {
                return ToolPreview{srv + " / " + remote_name, json_preview(a, 200)};
            };
            registry.add(std::move(t));
            added++;
        }

        servers_.push_back(server);
        if (report)
            report("mcp: " + cfg.name + " connected -- " + server->server_info() +
                   ", " + std::to_string(added) + " tool" + (added == 1 ? "" : "s"));
    }
}

void Manager::disconnect_all() {
    for (auto& s : servers_) s->disconnect();
    servers_.clear();
}

std::vector<std::string> Manager::status_lines() const {
    std::vector<std::string> out;
    for (const auto& s : servers_) {
        out.push_back(s->name() + "  " + (s->connected() ? "connected" : "disconnected") +
                      "  " + s->server_info() + "  (" +
                      std::to_string(s->tools().size()) + " tools)");
    }
    return out;
}

} // namespace ppcode::mcp

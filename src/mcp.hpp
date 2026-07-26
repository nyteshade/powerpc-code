// mcp.hpp -- Model Context Protocol client.
//
// Two transports:
//   stdio  -- fork/exec a server and speak newline-delimited JSON-RPC over pipes
//   http   -- POST JSON-RPC to a Streamable HTTP endpoint (typically a server
//             running on another machine, since this box has no Node.js)
//
// Requests are synchronous: send, then read until the matching id comes back,
// queueing any notifications that arrive in between.
#pragma once

#include "common.hpp"
#include "config.hpp"
#include "tools.hpp"

#include <memory>

namespace ppcode::mcp {

// The protocol revision we advertise. Servers that only speak an older one
// generally still work; we log whatever they report back.
inline constexpr const char* kProtocolVersion = "2025-06-18";

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool start(std::string* error) = 0;
    virtual bool send(const json& msg, std::string* error) = 0;
    // Blocks up to timeout_ms for one message. Returns false on timeout or EOF.
    virtual bool receive(json* out, int timeout_ms, std::string* error) = 0;
    virtual void stop() = 0;
    virtual bool alive() const = 0;
    // Anything the server wrote to stderr, for diagnostics.
    virtual std::string drain_stderr() { return ""; }
};

std::unique_ptr<Transport> make_stdio_transport(const McpServerConfig& cfg);
std::unique_ptr<Transport> make_http_transport(const McpServerConfig& cfg);

struct RemoteTool {
    std::string name;          // as the server calls it
    std::string description;
    json input_schema;
};

class Server {
public:
    explicit Server(McpServerConfig cfg) : cfg_(std::move(cfg)) {}
    ~Server();

    // Connect, handshake, and fetch the tool list.
    bool connect(std::string* error);
    void disconnect();

    const std::vector<RemoteTool>& tools() const { return tools_; }
    const std::string& name() const { return cfg_.name; }
    const std::string& server_info() const { return server_info_; }
    bool connected() const { return connected_; }

    // Invoke a remote tool. Never throws; failures come back as an error result.
    ToolResult call(const std::string& tool_name, const json& args, int timeout_ms);

private:
    McpServerConfig cfg_;
    std::unique_ptr<Transport> transport_;
    std::vector<RemoteTool> tools_;
    std::string server_info_;
    bool connected_ = false;
    int next_id_ = 1;

    bool request(const std::string& method, const json& params, json* result,
                 int timeout_ms, std::string* error);
    bool notify(const std::string& method, const json& params, std::string* error);
};

// Owns every configured server and publishes their tools into the registry.
class Manager {
public:
    // Connects each enabled server. Per-server failures are reported through
    // `report` and do not abort the rest -- one broken server should not stop
    // ppcode from starting.
    void connect_all(const std::vector<McpServerConfig>& servers,
                     ToolRegistry& registry,
                     const std::function<void(const std::string&)>& report);

    void disconnect_all();

    std::vector<std::string> status_lines() const;
    size_t server_count() const { return servers_.size(); }

private:
    std::vector<std::shared_ptr<Server>> servers_;
};

// Tool names are namespaced so two servers exposing "search" do not collide,
// and sanitised to the character set the API allows for function names.
std::string qualified_tool_name(const std::string& server, const std::string& tool);

} // namespace ppcode::mcp

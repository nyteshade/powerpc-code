// http.hpp -- libcurl wrapper with incremental Server-Sent Events support.
#pragma once

#include "common.hpp"

#include <atomic>

namespace ppcode::http {

struct Response {
    long status = 0;
    std::string body;      // empty for streaming calls
    std::string error;     // transport-level error, empty on success
    bool cancelled = false;

    // Response headers as "Name: value", status line dropped. Kept because MCP
    // over Streamable HTTP hands out its session id in one -- a client that
    // ignores it gets a 400 on the very next request and reports the server as
    // broken.
    std::vector<std::string> headers;

    // Case-insensitive lookup. Empty when absent.
    std::string header(const std::string& name) const;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

using Headers = std::vector<std::string>;   // "Name: value"

// One decoded SSE event. `data` is the concatenation of all data: lines in the
// event, joined with newlines, with the "data: " prefixes removed.
struct SseEvent {
    std::string event;   // from "event:", usually empty for OpenRouter
    std::string data;
};

// Return false from the callback to abort the transfer early.
using SseHandler = std::function<bool(const SseEvent&)>;

// Called once with the raw body for non-streaming requests.
Response get(const std::string& url, const Headers& headers, int timeout_s = 60);

Response post_json(const std::string& url, const Headers& headers,
                   const std::string& body, int timeout_s = 120);

// Streaming POST. `on_event` is invoked on the calling thread as data arrives.
// If `cancel` is non-null and becomes true, the transfer is aborted and
// Response::cancelled is set.
Response post_sse(const std::string& url, const Headers& headers,
                  const std::string& body, const SseHandler& on_event,
                  std::atomic<bool>* cancel = nullptr, int timeout_s = 600);

// Call once at startup, before any threads exist.
void global_init();
void global_cleanup();

// Human-readable libcurl version, for the /about display.
std::string version_string();

// ---------------------------------------------------------------------------
// SseParser -- fed arbitrary byte chunks, emits complete events. Exposed so it
// can be unit-tested independently of the network.
// ---------------------------------------------------------------------------
class SseParser {
public:
    // Returns false as soon as the handler asks to stop.
    bool feed(const char* data, size_t len, const SseHandler& on_event);
    // Flush a trailing event that was not terminated by a blank line.
    bool finish(const SseHandler& on_event);

private:
    std::string buf_;
    std::string event_;
    std::string data_;
    bool have_data_ = false;

    bool dispatch(const SseHandler& on_event);
    bool handle_line(const std::string& line, const SseHandler& on_event, bool* stop);
};

} // namespace ppcode::http

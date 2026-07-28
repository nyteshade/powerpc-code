#include "http.hpp"

#include <curl/curl.h>

#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::http {

// ---------------------------------------------------------------------------
// SseParser
// ---------------------------------------------------------------------------

bool SseParser::dispatch(const SseHandler& on_event) {
    if (!have_data_ && event_.empty()) return true;   // nothing meaningful
    SseEvent ev;
    ev.event = event_;
    ev.data = data_;
    event_.clear();
    data_.clear();
    have_data_ = false;
    return on_event(ev);
}

bool SseParser::handle_line(const std::string& line, const SseHandler& on_event,
                            bool* stop) {
    *stop = false;

    if (line.empty()) {                 // blank line terminates an event
        if (!dispatch(on_event)) { *stop = true; return false; }
        return true;
    }
    if (line[0] == ':') return true;    // comment, e.g. ": OPENROUTER PROCESSING"

    std::string field, value;
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        field = line;
    } else {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);  // one leading space
    }

    if (field == "data") {
        if (have_data_) data_ += "\n";
        data_ += value;
        have_data_ = true;
    } else if (field == "event") {
        event_ = value;
    }
    // "id" and "retry" are irrelevant for our use.
    return true;
}

bool SseParser::feed(const char* data, size_t len, const SseHandler& on_event) {
    buf_.append(data, len);

    size_t start = 0;
    while (true) {
        size_t nl = buf_.find('\n', start);
        if (nl == std::string::npos) break;

        std::string line = buf_.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = nl + 1;

        bool stop = false;
        handle_line(line, on_event, &stop);
        if (stop) {
            buf_.erase(0, start);
            return false;
        }
    }
    buf_.erase(0, start);
    return true;
}

bool SseParser::finish(const SseHandler& on_event) {
    if (!buf_.empty()) {
        std::string line = buf_;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        buf_.clear();
        bool stop = false;
        handle_line(line, on_event, &stop);
        if (stop) return false;
    }
    return dispatch(on_event);
}

// ---------------------------------------------------------------------------
// curl plumbing
// ---------------------------------------------------------------------------

namespace {

struct BodySink {
    std::string* out;
    std::atomic<bool>* cancel;
};

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sink = static_cast<BodySink*>(userdata);
    size_t n = size * nmemb;
    if (sink->cancel && sink->cancel->load()) return 0;   // abort
    sink->out->append(ptr, n);
    return n;
}

// Cap on the raw copy kept for error reporting. Successful streams are large
// and we never look at their raw bytes, so this only needs to hold an error
// payload.
constexpr size_t kRawErrorCap = 64 * 1024;

struct SseSink {
    SseParser* parser;
    const SseHandler* handler;
    std::atomic<bool>* cancel;
    std::string* raw;      // bounded copy, so non-SSE error bodies are visible
    bool stopped = false;
};

size_t write_sse(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sink = static_cast<SseSink*>(userdata);
    size_t n = size * nmemb;
    if (sink->cancel && sink->cancel->load()) {
        sink->stopped = true;
        return 0;
    }
    if (sink->raw->size() < kRawErrorCap)
        sink->raw->append(ptr, std::min(n, kRawErrorCap - sink->raw->size()));
    if (!sink->parser->feed(ptr, n, *sink->handler)) {
        sink->stopped = true;
        return 0;    // signals CURLE_WRITE_ERROR, which we treat as a clean stop
    }
    return n;
}

// MacPorts installs its CA bundle here. libcurl from the same prefix is normally
// compiled to find it, but being explicit costs nothing and avoids a confusing
// TLS failure if the build was configured differently.
const char* ca_bundle_path() {
    static std::string cached = [] () -> std::string {
        if (const char* env = std::getenv("CURL_CA_BUNDLE"); env && *env) return env;
        for (const char* p : {"/opt/local/share/curl/curl-ca-bundle.crt",
                              "/opt/local/etc/openssl/cert.pem",
                              "/opt/local/etc/openssl3/cert.pem"}) {
            std::error_code ec;
            if (fs::exists(p, ec)) return p;
        }
        return "";
    }();
    return cached.empty() ? nullptr : cached.c_str();
}

// Collects response headers. curl calls this once per line, including the
// status line and the blank line that ends the block; both are dropped.
size_t write_header(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<std::string>*>(userdata);
    size_t n = size * nmemb;
    std::string line(ptr, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    if (!line.empty() && line.find(':') != std::string::npos)
        out->push_back(line);
    return n;
}

curl_slist* build_headers(const Headers& headers) {
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    return list;
}

void apply_common(CURL* c, const std::string& url, curl_slist* hdrs, int timeout_s) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ppcode/0.1 (powerpc-apple-darwin9)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");   // let curl negotiate gzip
    if (const char* ca = ca_bundle_path()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
}

} // namespace

std::string Response::header(const std::string& name) const {
    std::string want = to_lower(name);
    for (const std::string& h : headers) {
        size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        if (to_lower(trim(h.substr(0, colon))) == want)
            return trim(h.substr(colon + 1));
    }

    return "";
}

void global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void global_cleanup() { curl_global_cleanup(); }

std::string version_string() {
    curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    if (!v) return "libcurl (unknown)";
    std::string s = std::string("libcurl ") + (v->version ? v->version : "?");
    if (v->ssl_version) s += std::string(" / ") + v->ssl_version;
    return s;
}

Response get(const std::string& url, const Headers& headers, int timeout_s) {
    Response r;
    CURL* c = curl_easy_init();
    if (!c) { r.error = "curl_easy_init failed"; return r; }

    curl_slist* hdrs = build_headers(headers);
    apply_common(c, url, hdrs, timeout_s);

    BodySink sink{&r.body, nullptr};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

Response post_json(const std::string& url, const Headers& headers,
                   const std::string& body, int timeout_s) {
    Response r;
    CURL* c = curl_easy_init();
    if (!c) { r.error = "curl_easy_init failed"; return r; }

    Headers all = headers;
    all.push_back("Content-Type: application/json");
    curl_slist* hdrs = build_headers(all);
    apply_common(c, url, hdrs, timeout_s);

    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

    BodySink sink{&r.body, nullptr};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

Response post_sse(const std::string& url, const Headers& headers,
                  const std::string& body, const SseHandler& on_event,
                  std::atomic<bool>* cancel, int timeout_s) {
    Response r;
    CURL* c = curl_easy_init();
    if (!c) { r.error = "curl_easy_init failed"; return r; }

    Headers all = headers;
    all.push_back("Content-Type: application/json");
    all.push_back("Accept: text/event-stream");
    curl_slist* hdrs = build_headers(all);
    apply_common(c, url, hdrs, timeout_s);

    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

    SseParser parser;
    SseSink sink{&parser, &on_event, cancel, &r.body, false};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sse);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);

    if (rc == CURLE_WRITE_ERROR && sink.stopped) {
        // We asked curl to stop; that is not a failure.
        r.cancelled = (cancel && cancel->load());
    } else if (rc != CURLE_OK) {
        r.error = curl_easy_strerror(rc);
    } else {
        parser.finish(on_event);
    }

    // On a non-2xx the body is a plain JSON error rather than SSE, so the
    // parser produced nothing. r.body holds the raw payload for the caller to
    // report. On success we drop it -- it is just a duplicate of the stream.
    if (r.status >= 200 && r.status < 300) r.body.clear();

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

} // namespace ppcode::http

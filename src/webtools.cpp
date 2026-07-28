#include "webtools.hpp"

#include "http.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace ppcode::web {

namespace {

const char* kUserAgent =
    "Mozilla/5.0 (Macintosh; PPC Mac OS X 10_5_8) ppcode/0.1";

bool iequal_at(const std::string& s, size_t pos, const char* lit) {
    size_t i = 0;
    for (; lit[i]; i++) {
        if (pos + i >= s.size()) return false;
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) !=
            std::tolower(static_cast<unsigned char>(lit[i])))
            return false;
    }
    return true;
}

// Skip an element and its contents entirely, e.g. <script>...</script>.
size_t skip_element(const std::string& html, size_t open_pos, const char* tag) {
    std::string close = std::string("</") + tag;
    size_t p = open_pos;
    while (p < html.size()) {
        if (html[p] == '<' && iequal_at(html, p, close.c_str())) {
            size_t gt = html.find('>', p);
            return gt == std::string::npos ? html.size() : gt + 1;
        }
        p++;
    }
    return html.size();
}

std::string decode_entities(const std::string& s) {
    static const struct { const char* name; const char* val; } named[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
        {"nbsp", " "}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
        {"hellip", "\xE2\x80\xA6"}, {"lsquo", "'"}, {"rsquo", "'"},
        {"ldquo", "\""}, {"rdquo", "\""}, {"copy", "(c)"}, {"reg", "(r)"},
        {"trade", "(tm)"}, {"middot", "\xC2\xB7"}, {"bull", "\xE2\x80\xA2"},
        {"times", "x"}, {"deg", "\xC2\xB0"}, {"euro", "\xE2\x82\xAC"},
        {"pound", "\xC2\xA3"}, {"laquo", "<<"}, {"raquo", ">>"},
    };

    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        std::string body = s.substr(i + 1, semi - i - 1);

        if (!body.empty() && body[0] == '#') {
            uint32_t cp = 0;
            if (body.size() > 2 && (body[1] == 'x' || body[1] == 'X'))
                cp = static_cast<uint32_t>(std::strtoul(body.c_str() + 2, nullptr, 16));
            else
                cp = static_cast<uint32_t>(std::strtoul(body.c_str() + 1, nullptr, 10));
            if (cp > 0 && cp <= 0x10FFFF) {
                out += utf8::encode(cp);
                i = semi;
                continue;
            }
            out += s[i];
            continue;
        }
        bool found = false;
        for (const auto& e : named) {
            if (body == e.name) { out += e.val; found = true; break; }
        }
        if (found) i = semi;
        else out += s[i];
    }
    return out;
}

bool is_block_tag(const std::string& name) {
    static const char* blocks[] = {
        "p", "div", "br", "hr", "li", "ul", "ol", "tr", "td", "th", "table",
        "h1", "h2", "h3", "h4", "h5", "h6", "section", "article", "header",
        "footer", "nav", "aside", "blockquote", "pre", "form", "figure", "dl",
        "dt", "dd", "main", "tbody", "thead",
    };
    for (const char* b : blocks) if (name == b) return true;
    return false;
}

} // namespace

std::string html_title(const std::string& html) {
    for (size_t i = 0; i + 6 < html.size(); i++) {
        if (html[i] == '<' && iequal_at(html, i, "<title")) {
            size_t gt = html.find('>', i);
            if (gt == std::string::npos) break;
            size_t end = html.find('<', gt + 1);
            if (end == std::string::npos) break;
            return trim(decode_entities(html.substr(gt + 1, end - gt - 1)));
        }
    }
    return "";
}

std::string html_to_text(const std::string& html) {
    std::string out;
    out.reserve(html.size() / 2);

    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') { out += html[i++]; continue; }

        // Comments and doctype
        if (html.compare(i, 4, "<!--") == 0) {
            size_t end = html.find("-->", i + 4);
            i = (end == std::string::npos) ? html.size() : end + 3;
            continue;
        }
        if (html.compare(i, 2, "<!") == 0) {
            size_t gt = html.find('>', i);
            i = (gt == std::string::npos) ? html.size() : gt + 1;
            continue;
        }
        // Elements whose contents are not prose
        for (const char* tag : {"script", "style", "head", "noscript", "svg",
                                "template", "iframe"}) {
            std::string open = std::string("<") + tag;
            if (iequal_at(html, i, open.c_str())) {
                // Only skip if it is really this tag (not e.g. <scriptfoo>).
                size_t after = i + open.size();
                if (after < html.size() &&
                    (html[after] == '>' || html[after] == ' ' || html[after] == '\t' ||
                     html[after] == '\n' || html[after] == '/')) {
                    i = skip_element(html, i, tag);
                    goto next;
                }
            }
        }
        {
            // An ordinary tag: note its name, then drop it.
            size_t p = i + 1;
            bool closing = (p < html.size() && html[p] == '/');
            if (closing) p++;
            size_t name_start = p;
            while (p < html.size() &&
                   (std::isalnum(static_cast<unsigned char>(html[p])) || html[p] == '-'))
                p++;
            std::string name = to_lower(html.substr(name_start, p - name_start));

            size_t gt = html.find('>', i);
            i = (gt == std::string::npos) ? html.size() : gt + 1;

            if (is_block_tag(name)) {
                if (!out.empty() && out.back() != '\n') out += '\n';
                // Give paragraphs and headings a blank line so the text reads.
                if (!closing && (name == "p" || name[0] == 'h' || name == "div" ||
                                 name == "blockquote" || name == "pre" ||
                                 name == "section" || name == "article")) {
                    if (out.size() >= 2 && out[out.size() - 2] != '\n') out += '\n';
                }
            }
        }
    next:;
    }

    out = decode_entities(out);

    // Collapse whitespace: at most one blank line, no trailing spaces.
    std::vector<std::string> lines = split(out, '\n');
    std::string result;
    int blanks = 0;
    for (std::string& l : lines) {
        // Squeeze interior runs of spaces and tabs.
        std::string sq;
        bool ws = false;
        for (char c : l) {
            if (c == ' ' || c == '\t' || c == '\r') {
                if (!ws) { sq += ' '; ws = true; }
            } else {
                sq += c;
                ws = false;
            }
        }
        sq = trim(sq);
        if (sq.empty()) {
            if (++blanks > 1) continue;
        } else {
            blanks = 0;
        }
        result += sq;
        result += '\n';
    }
    return trim(result);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

SearchBackend backend_from_string(const std::string& s, bool* ok) {
    if (ok) *ok = true;
    std::string v = to_lower(trim(s));
    if (v.empty() || v == "auto")                    return SearchBackend::Auto;
    if (v == "brave")                                return SearchBackend::Brave;
    if (v == "tavily")                               return SearchBackend::Tavily;
    if (v == "serper" || v == "google")              return SearchBackend::Serper;
    if (v == "searxng" || v == "searx")              return SearchBackend::SearxNG;
    if (v == "reference" || v == "wikipedia" || v == "none")
        return SearchBackend::Reference;
    if (ok) *ok = false;
    return SearchBackend::Auto;
}

std::string backend_name(SearchBackend b) {
    switch (b) {
        case SearchBackend::Auto:      return "auto";
        case SearchBackend::Brave:     return "brave";
        case SearchBackend::Tavily:    return "tavily";
        case SearchBackend::Serper:    return "serper";
        case SearchBackend::SearxNG:   return "searxng";
        case SearchBackend::Reference: return "reference";
    }
    return "?";
}

SearchConfig SearchConfig::from_env() {
    SearchConfig c;
    auto env = [](const char* n) -> std::string {
        const char* v = std::getenv(n);
        return (v && *v) ? std::string(v) : std::string();
    };
    c.brave_key   = env("BRAVE_SEARCH_API_KEY");
    c.tavily_key  = env("TAVILY_API_KEY");
    c.serper_key  = env("SERPER_API_KEY");
    c.searxng_url = env("PPCODE_SEARXNG_URL");
    if (std::string b = env("PPCODE_SEARCH_BACKEND"); !b.empty()) {
        bool ok = false;
        SearchBackend parsed = backend_from_string(b, &ok);
        if (ok) c.backend = parsed;
    }
    return c;
}

SearchConfig SearchConfig::from_config(const Config& cfg) {
    SearchConfig c = from_env();

    auto fill = [&cfg](std::string* slot, const char* key) {
        if (!slot->empty()) return;             // the environment already won
        auto it = cfg.search_keys.find(key);
        if (it != cfg.search_keys.end()) *slot = it->second;
    };
    fill(&c.brave_key,   "brave");
    fill(&c.tavily_key,  "tavily");
    fill(&c.serper_key,  "serper");
    fill(&c.searxng_url, "searxng");

    if (c.backend == SearchBackend::Auto && !cfg.search_backend.empty()) {
        bool ok = false;
        SearchBackend parsed = backend_from_string(cfg.search_backend, &ok);
        if (ok) c.backend = parsed;
    }
    if (cfg.web_max_results > 0) c.max_results = cfg.web_max_results;

    return c;
}

SearchBackend SearchConfig::resolve() const {
    if (backend != SearchBackend::Auto) return backend;
    // Prefer the backends that return real web results.
    if (!tavily_key.empty())  return SearchBackend::Tavily;
    if (!brave_key.empty())   return SearchBackend::Brave;
    if (!serper_key.empty())  return SearchBackend::Serper;
    if (!searxng_url.empty()) return SearchBackend::SearxNG;
    return SearchBackend::Reference;
}

std::string SearchConfig::availability_note() const {
    SearchBackend r = resolve();
    if (r != SearchBackend::Reference)
        return "search backend: " + backend_name(r);
    return
        "search backend: reference only (Wikipedia and DuckDuckGo instant "
        "answers). This finds encyclopaedic and definitional material but not "
        "general web pages. For real web search set one of TAVILY_API_KEY, "
        "BRAVE_SEARCH_API_KEY or SERPER_API_KEY, or point PPCODE_SEARXNG_URL at "
        "a SearXNG instance. Alternatively enable OpenRouter's own web plugin "
        "with \"web_search\": true in the config or job frontmatter, which needs "
        "no extra credentials.";
}

namespace {

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

SearchResult fail(const std::string& backend, const std::string& msg) {
    SearchResult r;
    r.backend = backend;
    r.error = msg;
    return r;
}

SearchResult search_tavily(const std::string& q, const SearchConfig& cfg) {
    json body = {{"api_key", cfg.tavily_key},
                 {"query", q},
                 {"max_results", cfg.max_results},
                 {"include_answer", true}};
    http::Response resp =
        http::post_json("https://api.tavily.com/search", {}, body.dump(), 60);
    if (!resp.error.empty()) return fail("tavily", resp.error);
    if (resp.status != 200)
        return fail("tavily", "HTTP " + std::to_string(resp.status) + ": " +
                                  json_preview(resp.body, 200));
    try {
        json j = json::parse(resp.body);
        SearchResult r;
        r.backend = "tavily";
        r.answer = jstr(j, "answer");
        if (const json* arr = jptr(j, "results"); arr && arr->is_array()) {
            for (const json& h : *arr) {
                r.hits.push_back({jstr(h, "title"), jstr(h, "url"),
                                  jstr(h, "content")});
            }
        }
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        return fail("tavily", e.what());
    }
}

SearchResult search_brave(const std::string& q, const SearchConfig& cfg) {
    http::Headers h = {"Accept: application/json",
                       "X-Subscription-Token: " + cfg.brave_key};
    std::string url = "https://api.search.brave.com/res/v1/web/search?q=" +
                      url_encode(q) + "&count=" + std::to_string(cfg.max_results);
    http::Response resp = http::get(url, h, 60);
    if (!resp.error.empty()) return fail("brave", resp.error);
    if (resp.status != 200)
        return fail("brave", "HTTP " + std::to_string(resp.status) + ": " +
                                 json_preview(resp.body, 200));
    try {
        json j = json::parse(resp.body);
        SearchResult r;
        r.backend = "brave";
        if (const json* web = jptr(j, "web")) {
            if (const json* arr = jptr(*web, "results"); arr && arr->is_array())
                for (const json& hit : *arr)
                    r.hits.push_back({jstr(hit, "title"), jstr(hit, "url"),
                                      jstr(hit, "description")});
        }
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        return fail("brave", e.what());
    }
}

SearchResult search_serper(const std::string& q, const SearchConfig& cfg) {
    http::Headers h = {"X-API-KEY: " + cfg.serper_key};
    json body = {{"q", q}, {"num", cfg.max_results}};
    http::Response resp =
        http::post_json("https://google.serper.dev/search", h, body.dump(), 60);
    if (!resp.error.empty()) return fail("serper", resp.error);
    if (resp.status != 200)
        return fail("serper", "HTTP " + std::to_string(resp.status) + ": " +
                                  json_preview(resp.body, 200));
    try {
        json j = json::parse(resp.body);
        SearchResult r;
        r.backend = "serper";
        if (const json* ab = jptr(j, "answerBox")) r.answer = jstr(*ab, "answer");
        if (const json* arr = jptr(j, "organic"); arr && arr->is_array())
            for (const json& hit : *arr)
                r.hits.push_back({jstr(hit, "title"), jstr(hit, "link"),
                                  jstr(hit, "snippet")});
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        return fail("serper", e.what());
    }
}

SearchResult search_searxng(const std::string& q, const SearchConfig& cfg) {
    std::string base = cfg.searxng_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base + "/search?format=json&q=" + url_encode(q);
    http::Response resp = http::get(url, {"Accept: application/json"}, 60);
    if (!resp.error.empty()) return fail("searxng", resp.error);
    if (resp.status != 200)
        return fail("searxng", "HTTP " + std::to_string(resp.status) +
                                   " (many public instances disable the JSON API)");
    try {
        json j = json::parse(resp.body);
        SearchResult r;
        r.backend = "searxng";
        if (const json* arr = jptr(j, "results"); arr && arr->is_array()) {
            int n = 0;
            for (const json& hit : *arr) {
                r.hits.push_back({jstr(hit, "title"), jstr(hit, "url"),
                                  jstr(hit, "content")});
                if (++n >= cfg.max_results) break;
            }
        }
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        return fail("searxng", e.what());
    }
}

// No credentials required. Wikipedia's search API plus DuckDuckGo's instant
// answer endpoint; both were verified reachable from this machine.
SearchResult search_reference(const std::string& q, const SearchConfig& cfg) {
    SearchResult r;
    r.backend = "reference";

    {
        std::string url =
            "https://api.duckduckgo.com/?format=json&no_html=1&skip_disambig=1&q=" +
            url_encode(q);
        http::Response resp = http::get(url, {}, 30);
        if (resp.error.empty() && resp.status == 200) {
            try {
                json j = json::parse(resp.body);
                std::string abstract = jstr(j, "AbstractText");
                if (abstract.empty()) abstract = jstr(j, "Abstract");
                if (!abstract.empty()) {
                    r.answer = abstract;
                    std::string src = jstr(j, "AbstractURL");
                    if (!src.empty())
                        r.hits.push_back({jstr(j, "Heading", "DuckDuckGo"), src,
                                          abstract});
                }
                if (const json* rel = jptr(j, "RelatedTopics");
                    rel && rel->is_array()) {
                    int n = 0;
                    for (const json& t : *rel) {
                        std::string text = jstr(t, "Text");
                        std::string u = jstr(t, "FirstURL");
                        if (text.empty() || u.empty()) continue;
                        r.hits.push_back({elide(text, 80), u, text});
                        if (++n >= 3) break;
                    }
                }
            } catch (const std::exception&) {
                // Non-fatal; Wikipedia may still answer.
            }
        }
    }

    {
        std::string url =
            "https://en.wikipedia.org/w/api.php?action=query&list=search&format=json"
            "&srlimit=" + std::to_string(cfg.max_results) + "&srsearch=" + url_encode(q);
        http::Response resp = http::get(url, {}, 30);
        if (resp.error.empty() && resp.status == 200) {
            try {
                json j = json::parse(resp.body);
                if (const json* qr = jptr(j, "query")) {
                    if (const json* arr = jptr(*qr, "search"); arr && arr->is_array()) {
                        for (const json& hit : *arr) {
                            std::string title = jstr(hit, "title");
                            if (title.empty()) continue;
                            std::string snippet = html_to_text(jstr(hit, "snippet"));
                            std::string page = title;
                            std::replace(page.begin(), page.end(), ' ', '_');
                            r.hits.push_back(
                                {title, "https://en.wikipedia.org/wiki/" + url_encode(page),
                                 snippet});
                        }
                    }
                }
            } catch (const std::exception&) {
            }
        }
    }

    if (r.hits.empty() && r.answer.empty()) {
        r.error = "no results from the credential-free reference backends. " +
                  cfg.availability_note();
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace

SearchResult search(const std::string& query, const SearchConfig& cfg) {
    std::string q = trim(query);
    if (q.empty()) return fail("none", "empty query");

    switch (cfg.resolve()) {
        case SearchBackend::Tavily:  return search_tavily(q, cfg);
        case SearchBackend::Brave:   return search_brave(q, cfg);
        case SearchBackend::Serper:  return search_serper(q, cfg);
        case SearchBackend::SearxNG: return search_searxng(q, cfg);
        case SearchBackend::Reference:
        case SearchBackend::Auto:
        default:                     return search_reference(q, cfg);
    }
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

void add_tools(ToolRegistry& registry, const SearchConfig& cfg) {
    {
        Tool t;
        t.spec.name = "web_fetch";
        t.spec.description =
            "Fetch a URL over HTTP(S) and return its content as readable text. "
            "HTML is stripped to prose. Use this to read documentation, source "
            "files, README files and API responses.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "url":      {"type": "string",  "description": "Absolute http:// or https:// URL."},
                "max_bytes":{"type": "integer", "description": "Truncate the extracted text to this many bytes. Default 40000."},
                "raw":      {"type": "boolean", "description": "Return the response body verbatim instead of extracting text. Use for JSON or source files."}
            },
            "required": ["url"]
        })");
        // Fetching reaches outside the machine, so it goes through the gate.
        t.kind = ToolKind::Execute;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"web_fetch", jstr(a, "url")};
        };
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            std::string url = jstr(a, "url");
            if (url.empty()) return ToolResult::err("'url' is required");
            if (!starts_with(url, "http://") && !starts_with(url, "https://"))
                return ToolResult::err("url must start with http:// or https://");

            int64_t max_bytes = jint(a, "max_bytes", 40000);
            if (max_bytes <= 0 || max_bytes > 500000) max_bytes = 40000;
            bool raw = jbool(a, "raw", false);

            if (ctx.note) ctx.note("fetching " + elide(url, 100));

            http::Response r = http::get(url, {std::string("User-Agent: ") + kUserAgent},
                                         60);
            if (!r.error.empty())
                return ToolResult::err(
                    "could not fetch " + url + ": " + r.error +
                    ". If this looks like a TLS failure, this machine's HTTPS "
                    "support depends on MacPorts curl/OpenSSL being present.");
            if (r.status < 200 || r.status >= 300)
                return ToolResult::err("HTTP " + std::to_string(r.status) +
                                       " fetching " + url);

            std::string body = raw ? r.body : html_to_text(r.body);
            std::string title = raw ? "" : html_title(r.body);
            bool truncated = false;
            if (static_cast<int64_t>(body.size()) > max_bytes) {
                body = body.substr(0, static_cast<size_t>(max_bytes));
                truncated = true;
            }
            std::string out = url;
            if (!title.empty()) out += "\n" + title;
            out += "\n\n" + body;
            if (truncated)
                out += "\n\n[truncated at " + std::to_string(max_bytes) +
                       " bytes; request more with max_bytes]";
            return ToolResult::ok(out);
        };
        registry.add(std::move(t));
    }

    {
        Tool t;
        t.spec.name = "web_search";
        t.spec.description =
            "Search the web and return titles, URLs and snippets. Follow up with "
            "web_fetch to read a result in full. " + cfg.availability_note();
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "query":       {"type": "string",  "description": "What to search for."},
                "max_results": {"type": "integer", "description": "Maximum results to return. Default 6."}
            },
            "required": ["query"]
        })");
        // A search reads the public web and changes nothing, so it is not gated.
        // It used to be, and a turn that searched five times asked five times --
        // which taught nobody anything and made the answer arrive a minute late.
        // web_fetch stays behind the gate: it takes a URL the model chose, and a
        // URL can carry data outward as easily as bring it back.
        t.kind = ToolKind::Read;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"web_search", jstr(a, "query")};
        };
        SearchConfig captured = cfg;
        t.handler = [captured](const json& a, ToolContext& ctx) -> ToolResult {
            std::string q = jstr(a, "query");
            if (q.empty()) return ToolResult::err("'query' is required");

            SearchConfig c = captured;
            if (int64_t n = jint(a, "max_results", 0); n > 0 && n <= 25)
                c.max_results = static_cast<int>(n);

            if (ctx.note) ctx.note("searching: " + elide(q, 80));

            SearchResult r = search(q, c);
            if (!r.ok) return ToolResult::err(r.error);

            std::string out = "Results from " + r.backend + " for: " + q + "\n";
            if (!r.answer.empty()) out += "\nSummary: " + r.answer + "\n";
            out += "\n";
            int n = 1;
            for (const SearchHit& h : r.hits) {
                out += std::to_string(n++) + ". " + h.title + "\n   " + h.url + "\n";
                if (!h.snippet.empty())
                    out += "   " + elide(json_preview(h.snippet, 400), 400) + "\n";
            }
            return ToolResult::ok(out);
        };
        registry.add(std::move(t));
    }
}

} // namespace ppcode::web

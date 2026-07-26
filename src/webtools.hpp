// webtools.hpp -- fetching and searching the web.
//
// This machine can reach the modern internet only because MacPorts supplies a
// current curl and OpenSSL; the stock Leopard ones cannot complete a handshake
// with most hosts. So every entry point here reports clearly when the transport
// is the problem rather than the request.
//
// Search has no single good answer without credentials: DuckDuckGo's HTML
// endpoint now serves a bot check, so scraping it is not viable. The backends
// below range from "works with no setup but is limited" to "needs an API key
// and is genuinely good".
#pragma once

#include "common.hpp"
#include "tools.hpp"

namespace ppcode::web {

// Convert an HTML document to readable plain text: drops script/style/head,
// turns block elements into line breaks, decodes entities, collapses runs of
// whitespace. Not a browser -- good enough to read documentation.
std::string html_to_text(const std::string& html);

// Extract the <title>, if any.
std::string html_title(const std::string& html);

enum class SearchBackend {
    Auto,        // pick the best configured option
    Brave,       // needs BRAVE_SEARCH_API_KEY
    Tavily,      // needs TAVILY_API_KEY
    Serper,      // needs SERPER_API_KEY
    SearxNG,     // needs a base URL (PPCODE_SEARXNG_URL)
    Reference,   // no credentials: Wikipedia + DuckDuckGo instant answers
};

SearchBackend backend_from_string(const std::string& s, bool* ok);
std::string backend_name(SearchBackend b);

struct SearchConfig {
    SearchBackend backend = SearchBackend::Auto;
    std::string brave_key;
    std::string tavily_key;
    std::string serper_key;
    std::string searxng_url;
    int max_results = 6;

    // Fill keys from the environment.
    static SearchConfig from_env();

    // Which backend Auto would actually choose, and why not the others.
    SearchBackend resolve() const;
    std::string availability_note() const;
};

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

struct SearchResult {
    bool ok = false;
    std::string error;
    std::string backend;
    std::vector<SearchHit> hits;
    std::string answer;      // some backends return a direct answer
};

SearchResult search(const std::string& query, const SearchConfig& cfg);

// Register web_fetch and web_search into a tool registry. `cfg` is captured.
void add_tools(ToolRegistry& registry, const SearchConfig& cfg);

} // namespace ppcode::web

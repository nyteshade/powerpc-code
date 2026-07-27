// provider.hpp -- which service we are talking to.
//
// Everything ppcode talks to speaks the OpenAI chat-completions shape, so a
// provider is mostly data: a base URL, where the key comes from, and which
// optional parts of the protocol the service actually implements. Adding one
// should be a table entry, not a subclass -- there is no behaviour here that
// varies enough to justify a vtable, and a table can be inspected, listed in
// --help and tested without a network.
//
// The differences that do matter are the ones flagged below: OpenRouter puts
// routing preferences and a cost figure in the exchange, DeepSeek does neither
// and needs its prices supplied locally, and a local server needs no key at all.
#pragma once

#include "common.hpp"

namespace ppcode {

// Static metadata for a model, for providers whose /models endpoint returns
// bare ids. DeepSeek's returns exactly {"id", "object", "owned_by"} -- no
// context length, no prices -- so without this the context budget and the
// spend cap would both be guessing.
struct ModelDefaults {
    std::string id;
    int64_t context_length = 0;
    int64_t max_completion_tokens = 0;
    double prompt_cost = 0.0;           // USD per token
    double completion_cost = 0.0;
    double cached_prompt_cost = 0.0;    // USD per token on a cache hit
    bool supports_tools = true;
    bool supports_reasoning = false;
    bool supports_images = false;
    std::string description;
};

struct Provider {
    std::string id;                 // as given to --provider
    std::string name;               // for humans
    std::string base_url;

    // Tried in order. The first that is set wins.
    std::vector<std::string> key_env;

    // A shell fragment under $HOME holding `export NAME=value`, read when the
    // environment has nothing. A GUI application launched from the Finder
    // inherits no shell environment at all, so without this the key would have
    // to be typed into the interface on every machine.
    std::string key_file;

    bool needs_key = true;

    // --- what the service actually implements ------------------------------

    bool has_models_endpoint = true;
    // True when /models carries pricing, context length and modalities.
    // False means merge `models` below over whatever ids come back.
    bool models_have_metadata = false;
    bool has_credits = false;           // /credits
    bool supports_routing = false;      // the "provider" request field
    bool supports_plugins = false;      // the "plugins" request field, web search
    bool cost_in_usage = false;         // usage.cost comes back per request
    bool attribution_headers = false;   // HTTP-Referer / X-Title

    std::string default_model;
    std::vector<std::string> favourites;   // ascending capability and cost
    std::vector<ModelDefaults> models;     // static metadata

    const ModelDefaults* model_defaults(const std::string& id) const;
};

// Registry. Unknown ids return null rather than falling back silently, so a
// typo in --provider is reported instead of quietly using OpenRouter.
const Provider* find_provider(const std::string& id);
std::vector<const Provider*> all_providers();
const Provider& default_provider();

// Comma-separated ids, for help text and error messages.
std::string provider_id_list();

// The key for a provider: environment first, then its key file. Empty when
// there is none, which is not an error for a provider that needs no key.
std::string resolve_api_key(const Provider& p);

// Cost of an exchange in USD for providers that do not report one. Returns 0
// when the model's prices are unknown, which the caller must treat as "not
// known" rather than "free".
double estimate_cost(const Provider& p, const std::string& model,
                     int64_t prompt_tokens, int64_t completion_tokens,
                     int64_t cached_prompt_tokens);

} // namespace ppcode

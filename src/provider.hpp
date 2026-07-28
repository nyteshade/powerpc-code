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

    // Defined by the user in the config file rather than compiled in. Only
    // these can be edited or deleted from the interface.
    bool custom = false;
    // A custom provider that has since been deleted. It stays in the table
    // because pointers to it have been handed out, and stops being listed.
    bool hidden = false;

    const ModelDefaults* model_defaults(const std::string& id) const;
};

// Registry. Unknown ids return null rather than falling back silently, so a
// typo in --provider is reported instead of quietly using OpenRouter.
const Provider* find_provider(const std::string& id);
std::vector<const Provider*> all_providers();
const Provider& default_provider();

// Comma-separated ids, for help text and error messages.
std::string provider_id_list();

// ---------------------------------------------------------------------------
// User-defined providers
//
// Anything speaking the OpenAI chat shape can be talked to, and the table above
// cannot anticipate which one someone will want -- a second LM Studio, a
// llama.cpp server, a company gateway. So the table is extensible from the
// config file's "custom_providers", and the interface can add to it.
//
// Pointers returned by find_provider() must stay valid for the life of the
// process: a Config holds one, and reloading the config re-registers the whole
// set. So entries are updated in place and never erased; one that goes away is
// marked hidden and stops being listed.
// ---------------------------------------------------------------------------

// Fill in the defaults a hand-written provider needs but nobody wants to type.
// `id` is sanitised to lower-case alphanumerics, '-' and '_'.
Provider make_custom_provider(const std::string& id, const std::string& name,
                              const std::string& base_url,
                              const std::string& default_model, bool needs_key);

// Replace the user-defined set with exactly this list.
void set_custom_providers(const std::vector<Provider>& list);

// The user-defined ones, in the order they were registered.
std::vector<const Provider*> custom_providers();

// Sanitise an id the way make_custom_provider() does, so a caller can check
// for a collision before offering to create one.
std::string sanitise_provider_id(const std::string& id);

// The key for a provider: environment first, then its key file. Empty when
// there is none, which is not an error for a provider that needs no key.
std::string resolve_api_key(const Provider& p);

// The absolute path of a provider's key file, or empty when it has none.
std::string api_key_path(const Provider& p);

// Where the key actually came from, for an interface that has to explain why
// typing one changed nothing: "environment (DEEPSEEK_API_KEY)", the path of the
// key file, or empty when there is no key at all. The environment wins over the
// file, so a stale exported variable is worth naming rather than hiding.
std::string api_key_source(const Provider& p);

// Write a key to the provider's key file, owner-readable only. This is the one
// per-provider store the command line already reads, so a key set in the
// application is the same key the CLI will find -- the alternative, a second
// copy in config.json, is how a key comes to be configured in one place and
// missing in the other.
bool save_api_key(const Provider& p, const std::string& key, std::string* error);

// Cost of an exchange in USD for providers that do not report one. Returns 0
// when the model's prices are unknown, which the caller must treat as "not
// known" rather than "free".
double estimate_cost(const Provider& p, const std::string& model,
                     int64_t prompt_tokens, int64_t completion_tokens,
                     int64_t cached_prompt_tokens);

} // namespace ppcode

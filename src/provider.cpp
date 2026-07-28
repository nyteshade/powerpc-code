#include "provider.hpp"

#include <cctype>
#include <cstdlib>
#include <deque>
#include <filesystem>

namespace ppcode {

namespace {

// Prices are USD per token, so the per-million figures from each service's
// pricing page are divided here once rather than at every call site.
constexpr double kPerM = 1e-6;

std::vector<Provider> build_registry() {
    std::vector<Provider> out;

    // --- OpenRouter --------------------------------------------------------
    {
        Provider p;
        p.id = "openrouter";
        p.name = "OpenRouter";
        p.base_url = "https://openrouter.ai/api/v1";
        p.key_env = {"OPENROUTER_AI_API_KEY", "OPENROUTER_API_KEY"};
        p.key_file = ".local/keys/openrouter";
        p.has_models_endpoint = true;
        p.models_have_metadata = true;   // pricing, context and modalities
        p.has_credits = true;
        p.supports_routing = true;
        p.supports_plugins = true;
        p.cost_in_usage = true;          // usage.cost per request
        p.attribution_headers = true;
        p.default_model = "anthropic/claude-sonnet-5";
        // Ascending capability and cost, and pinned so the ids never have to be
        // recalled.
        p.favourites = {"deepseek/deepseek-v4-pro", "z-ai/glm-5.2",
                        "moonshotai/kimi-k3"};
        out.push_back(p);
    }

    // --- DeepSeek ----------------------------------------------------------
    {
        Provider p;
        p.id = "deepseek";
        p.name = "DeepSeek";
        p.base_url = "https://api.deepseek.com";
        p.key_env = {"DEEPSEEK_API_KEY"};
        p.key_file = ".local/keys/deepseek";
        p.has_models_endpoint = true;
        // /models returns {"id","object","owned_by"} and nothing else.
        p.models_have_metadata = false;
        p.has_credits = false;
        p.supports_routing = false;
        p.supports_plugins = false;
        // No cost in the usage block, so it is computed from the table below.
        p.cost_in_usage = false;
        p.attribution_headers = false;
        p.default_model = "deepseek-v4-flash";
        p.favourites = {"deepseek-v4-flash", "deepseek-v4-pro"};

        {
            ModelDefaults m;
            m.id = "deepseek-v4-flash";
            m.context_length = 1000000;
            m.max_completion_tokens = 384000;
            m.prompt_cost = 0.14 * kPerM;
            m.completion_cost = 0.28 * kPerM;
            m.cached_prompt_cost = 0.0028 * kPerM;
            m.supports_tools = true;
            m.supports_reasoning = true;      // thinking mode, on by default
            m.description = "Fast DeepSeek V4, 1M context.";
            p.models.push_back(m);
        }
        {
            ModelDefaults m;
            m.id = "deepseek-v4-pro";
            m.context_length = 1000000;
            m.max_completion_tokens = 384000;
            m.prompt_cost = 0.435 * kPerM;
            m.completion_cost = 0.87 * kPerM;
            m.cached_prompt_cost = 0.003625 * kPerM;
            m.supports_tools = true;
            m.supports_reasoning = true;
            m.description = "DeepSeek V4 Pro, 1M context.";
            p.models.push_back(m);
        }

        out.push_back(p);
    }

    // --- LM Studio ---------------------------------------------------------
    //
    // A local server, so no key and no prices. Its /models lists whatever has
    // been loaded, which is why there is no default model worth naming and no
    // static table: the answer is whatever is on the machine.
    {
        Provider p;
        p.id = "lmstudio";
        p.name = "LM Studio";
        p.base_url = "http://localhost:1234/v1";
        p.key_env = {"LMSTUDIO_API_KEY"};   // honoured if set; not required
        // A key file even though no key is needed: an LM Studio reached across
        // the network may well sit behind a proxy that wants one, and there has
        // to be somewhere to put it.
        p.key_file = ".local/keys/lmstudio";
        p.needs_key = false;
        p.has_models_endpoint = true;
        p.models_have_metadata = false;
        p.has_credits = false;
        p.supports_routing = false;
        p.supports_plugins = false;
        p.cost_in_usage = false;            // local: nothing to charge
        p.attribution_headers = false;
        out.push_back(p);
    }

    return out;
}

// A deque, not a vector: appending to a vector moves every element, and
// find_provider() hands out pointers that a Config keeps for its lifetime.
// Appending to a deque leaves the existing elements exactly where they are.
std::deque<Provider>& registry() {
    static std::deque<Provider> r = [] {
        std::deque<Provider> d;
        for (const Provider& p : build_registry()) d.push_back(p);

        return d;
    }();

    return r;
}

// Pull `NAME=value` out of a shell fragment. Deliberately not a shell: this
// reads an assignment and nothing else, so a key file cannot run anything.
std::string key_from_file(const std::string& rel_path,
                          const std::vector<std::string>& names) {
    const char* home = std::getenv("HOME");
    if (!home || !*home || rel_path.empty()) return "";

    std::string text, err;
    if (!read_file_text(std::string(home) + "/" + rel_path, &text, &err))
        return "";

    for (const std::string& raw : split(text, '\n')) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (starts_with(line, "export ")) line = trim(line.substr(7));

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string name = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        // Strip one layer of quoting, which is how these files are usually
        // written.
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);

        for (const std::string& want : names)
            if (name == want && !value.empty()) return value;
    }

    return "";
}

} // namespace

const ModelDefaults* Provider::model_defaults(const std::string& mid) const {
    for (const ModelDefaults& m : models)
        if (m.id == mid) return &m;

    return nullptr;
}

const Provider* find_provider(const std::string& id) {
    std::string want = to_lower(trim(id));
    if (want.empty()) return &default_provider();

    for (const Provider& p : registry())
        if (p.id == want && !p.hidden) return &p;

    return nullptr;
}

std::vector<const Provider*> all_providers() {
    std::vector<const Provider*> out;
    for (const Provider& p : registry())
        if (!p.hidden) out.push_back(&p);

    return out;
}

std::vector<const Provider*> custom_providers() {
    std::vector<const Provider*> out;
    for (const Provider& p : registry())
        if (p.custom && !p.hidden) out.push_back(&p);

    return out;
}

const Provider& default_provider() { return registry()[0]; }

std::string provider_id_list() {
    std::string out;
    for (const Provider& p : registry()) {
        if (p.hidden) continue;
        if (!out.empty()) out += ", ";
        out += p.id;
    }

    return out;
}

std::string sanitise_provider_id(const std::string& id) {
    std::string out;
    for (char c : to_lower(trim(id))) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            out += c;
        else if (c == ' ' || c == '.' || c == '/')
            out += '-';
    }

    return out;
}

Provider make_custom_provider(const std::string& id, const std::string& name,
                              const std::string& base_url,
                              const std::string& default_model, bool needs_key) {
    Provider p;
    p.custom = true;
    p.id = sanitise_provider_id(id);
    p.name = trim(name).empty() ? p.id : trim(name);

    // The endpoint paths are appended to this, so a trailing slash produces
    // ".../v1//models" -- which some servers answer and others reject, making
    // it exactly the sort of thing to normalise once rather than debug twice.
    p.base_url = trim(base_url);
    while (p.base_url.size() > 1 && p.base_url.back() == '/') p.base_url.pop_back();

    // The environment variable name a key would be exported under, derived from
    // the id so it is predictable enough to put in a shell profile.
    std::string env;
    for (char c : p.id)
        env += std::isalnum(static_cast<unsigned char>(c))
                   ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                   : '_';
    p.key_env = {env + "_API_KEY"};
    p.key_file = ".local/keys/" + p.id;
    p.needs_key = needs_key;

    // What an unknown OpenAI-compatible endpoint can be assumed to do, and no
    // more. /models is near-universal; the rest -- routing, plugins, credits,
    // a cost in the usage block -- are one vendor's extensions each, and asking
    // for them where they are not implemented is how a request comes back 400.
    p.has_models_endpoint = true;
    p.models_have_metadata = false;
    p.has_credits = false;
    p.supports_routing = false;
    p.supports_plugins = false;
    p.cost_in_usage = false;
    p.attribution_headers = false;
    p.default_model = trim(default_model);

    return p;
}

void set_custom_providers(const std::vector<Provider>& list) {
    std::deque<Provider>& r = registry();

    // Anything user-defined is hidden first, then un-hidden as the new list
    // reinstates it. Nothing is erased: a Config out there holds a pointer.
    for (Provider& p : r)
        if (p.custom) p.hidden = true;

    for (const Provider& want : list) {
        if (want.id.empty()) continue;

        bool replaced = false;
        for (Provider& p : r) {
            if (p.id != want.id) continue;
            // A built-in of the same id wins: the table is the contract, and
            // silently shadowing "openrouter" with a half-filled entry would
            // be a very confusing way to lose routing and cost reporting.
            if (!p.custom) { replaced = true; break; }

            std::string id = p.id;
            p = want;
            p.id = id;
            p.custom = true;
            p.hidden = false;
            replaced = true;
            break;
        }
        if (!replaced) r.push_back(want);
    }
}

std::string resolve_api_key(const Provider& p) {
    for (const std::string& name : p.key_env) {
        if (const char* v = std::getenv(name.c_str()); v && *v) return v;
    }

    return key_from_file(p.key_file, p.key_env);
}

std::string api_key_path(const Provider& p) {
    const char* home = std::getenv("HOME");
    if (!home || !*home || p.key_file.empty()) return "";

    return std::string(home) + "/" + p.key_file;
}

std::string api_key_source(const Provider& p) {
    for (const std::string& name : p.key_env) {
        if (const char* v = std::getenv(name.c_str()); v && *v)
            return "environment (" + name + ")";
    }
    if (!key_from_file(p.key_file, p.key_env).empty()) return api_key_path(p);

    return "";
}

bool save_api_key(const Provider& p, const std::string& key, std::string* error) {
    std::string k = trim(key);
    if (k.empty()) {
        if (error) *error = "the key is empty";

        return false;
    }

    std::string path = api_key_path(p);
    if (path.empty()) {
        if (error) *error = "no key file is defined for " + p.name;

        return false;
    }

    // The name the file is read back under. resolve_api_key() looks for any of
    // key_env, so writing the first one is enough; a provider with none at all
    // would be unreadable, hence the derived fallback.
    std::string name;
    if (!p.key_env.empty()) name = p.key_env.front();

    else {
        for (size_t i = 0; i < p.id.size(); i++)
            name += static_cast<char>(std::toupper(
                static_cast<unsigned char>(p.id[i])));
        name += "_API_KEY";
    }

    std::error_code ec;
    std::filesystem::path fp(path);
    if (fp.has_parent_path()) {
        std::filesystem::create_directories(fp.parent_path(), ec);
        if (ec) {
            if (error) *error = "could not create " +
                                fp.parent_path().string() + ": " + ec.message();

            return false;
        }
    }

    // Written as a shell fragment because that is what these files have always
    // been: the same file can be sourced from a profile, which is how the key
    // reaches a terminal as well as the application.
    if (!write_file_text(path, "export " + name + "=" + k + "\n", error))
        return false;

    // A credential. Doing this after the write leaves no window in which the
    // file exists with the default mask.
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);

    return true;
}

double estimate_cost(const Provider& p, const std::string& model,
                     int64_t prompt_tokens, int64_t completion_tokens,
                     int64_t cached_prompt_tokens) {
    const ModelDefaults* m = p.model_defaults(model);
    if (!m) return 0.0;

    // Cached tokens are billed at the cache-hit rate and are already included
    // in prompt_tokens, so they are taken out of the full-price count rather
    // than added on top.
    int64_t cached = cached_prompt_tokens;
    if (cached > prompt_tokens) cached = prompt_tokens;
    int64_t fresh = prompt_tokens - cached;

    return fresh * m->prompt_cost + cached * m->cached_prompt_cost +
           completion_tokens * m->completion_cost;
}

} // namespace ppcode

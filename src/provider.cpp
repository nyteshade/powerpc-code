#include "provider.hpp"

#include <cstdlib>

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

const std::vector<Provider>& registry() {
    static const std::vector<Provider> r = build_registry();

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
        if (p.id == want) return &p;

    return nullptr;
}

std::vector<const Provider*> all_providers() {
    std::vector<const Provider*> out;
    for (const Provider& p : registry()) out.push_back(&p);

    return out;
}

const Provider& default_provider() { return registry()[0]; }

std::string provider_id_list() {
    std::string out;
    for (const Provider& p : registry()) {
        if (!out.empty()) out += ", ";
        out += p.id;
    }

    return out;
}

std::string resolve_api_key(const Provider& p) {
    for (const std::string& name : p.key_env) {
        if (const char* v = std::getenv(name.c_str()); v && *v) return v;
    }

    return key_from_file(p.key_file, p.key_env);
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

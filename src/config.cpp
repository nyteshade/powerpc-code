#include "config.hpp"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode {

std::string Config::default_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/ppcode/config.json";
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/ppcode/config.json";
    return "./ppcode-config.json";
}

std::string Config::key_from_env() {
    // Kept for callers that just want "the key for the default provider".
    // Per-provider resolution lives in provider.cpp, which also reads the key
    // files -- a GUI application launched from the Finder inherits no shell
    // environment, so the environment alone is not enough.
    return resolve_api_key(default_provider());
}

bool Config::use_provider(const std::string& id, bool model_was_explicit,
                          bool base_url_was_explicit) {
    const Provider* p = find_provider(id);
    if (!p) return false;

    provider_info = p;
    provider_id = p->id;

    // Only fill in what the user has not already decided. A --model or a
    // base_url in the config file must survive switching provider, or
    // overriding either becomes impossible.
    if (!base_url_was_explicit) base_url = p->base_url;
    if (!model_was_explicit && !p->default_model.empty()) model = p->default_model;

    // Keys for every provider are resolved up front, so switching at runtime
    // does not need another trip to the filesystem.
    for (const Provider* q : all_providers()) {
        std::string k = resolve_api_key(*q);
        if (!k.empty()) api_keys[q->id] = k;
    }

    std::map<std::string, std::string>::const_iterator it = api_keys.find(p->id);
    if (it != api_keys.end()) api_key = it->second;
    else if (p->needs_key) api_key.clear();

    return true;
}

static void parse_mcp(const json& arr, Config& cfg, std::vector<std::string>* warnings) {
    if (!arr.is_array()) return;
    for (const json& s : arr) {
        if (!s.is_object()) continue;
        McpServerConfig m;
        m.name      = jstr(s, "name");
        m.transport = jstr(s, "transport", "stdio");
        m.command   = jstr(s, "command");
        m.url       = jstr(s, "url");
        m.enabled   = jbool(s, "enabled", true);

        if (const json* a = jptr(s, "args"); a && a->is_array()) {
            for (const json& x : *a)
                if (x.is_string()) m.args.push_back(x.get<std::string>());
        }
        if (const json* e = jptr(s, "env"); e && e->is_object()) {
            for (auto it = e->begin(); it != e->end(); ++it)
                if (it.value().is_string()) m.env[it.key()] = it.value().get<std::string>();
        }
        if (const json* h = jptr(s, "headers"); h && h->is_object()) {
            for (auto it = h->begin(); it != h->end(); ++it)
                if (it.value().is_string()) m.headers[it.key()] = it.value().get<std::string>();
        }

        if (m.name.empty()) {
            if (warnings) warnings->push_back("mcp server with no name ignored");
            continue;
        }
        if (m.transport == "stdio" && m.command.empty()) {
            if (warnings) warnings->push_back("mcp server '" + m.name + "' has no command");
            continue;
        }
        if (m.transport == "http" && m.url.empty()) {
            if (warnings) warnings->push_back("mcp server '" + m.name + "' has no url");
            continue;
        }
        cfg.mcp_servers.push_back(std::move(m));
    }
}

Config Config::load(const std::string& explicit_path, std::vector<std::string>* warnings) {
    Config cfg;
    // Whether the config file named these, so selecting a provider does not
    // overwrite a deliberate choice.
    bool model_explicit = false;
    bool base_url_explicit = false;
    cfg.config_path = explicit_path.empty() ? default_path() : expand_user(explicit_path);

    std::string text;
    if (read_file_text(cfg.config_path, &text, nullptr)) {
        try {
            // Permit // and /* */ comments -- hand-edited config files
            // invariably grow them.
            json j = json::parse(text, nullptr, true, true);

            cfg.model         = jstr(j, "model", cfg.model);
            if (jptr(j, "base_url")) base_url_explicit = true;
            cfg.base_url      = jstr(j, "base_url", cfg.base_url);
            if (jptr(j, "model")) model_explicit = true;
            cfg.provider_id   = jstr(j, "provider_id", cfg.provider_id);
            cfg.system_prompt = jstr(j, "system_prompt", cfg.system_prompt);
            cfg.temperature   = jnum(j, "temperature", cfg.temperature);
            cfg.max_tokens    = static_cast<int>(jint(j, "max_tokens", cfg.max_tokens));
            cfg.max_turns     = static_cast<int>(jint(j, "max_turns", cfg.max_turns));
            cfg.referer       = jstr(j, "referer", cfg.referer);
            cfg.title         = jstr(j, "title", cfg.title);
            cfg.unicode       = jbool(j, "unicode", cfg.unicode);
            cfg.color         = jbool(j, "color", cfg.color);
            cfg.auto_approve_reads =
                jbool(j, "auto_approve_reads", cfg.auto_approve_reads);
            cfg.yolo          = jbool(j, "yolo", cfg.yolo);
            cfg.cache_mode    = jstr(j, "cache_mode", cfg.cache_mode);
            cfg.max_retries   = static_cast<int>(jint(j, "max_retries", cfg.max_retries));
            cfg.max_cost      = jnum(j, "max_cost", cfg.max_cost);
            cfg.web_search    = jbool(j, "web_search", cfg.web_search);
            cfg.web_max_results =
                static_cast<int>(jint(j, "web_max_results", cfg.web_max_results));

            // An API key in the config file is supported but discouraged.
            if (const json* k = jptr(j, "api_key"); k && k->is_string())
                cfg.api_key = k->get<std::string>();

            if (const json* m = jptr(j, "mcp_servers")) parse_mcp(*m, cfg, warnings);
        } catch (const std::exception& e) {
            if (warnings)
                warnings->push_back("config parse error in " + cfg.config_path + ": " +
                                    e.what() + " (using defaults)");
        }
    }

    // The provider decides the base URL, the default model and where the key
    // comes from, so it is resolved before either is finalised. Whether the
    // file named them explicitly decides what may be overwritten.
    if (!cfg.use_provider(cfg.provider_id, model_explicit, base_url_explicit)) {
        if (warnings)
            warnings->push_back("unknown provider \"" + cfg.provider_id +
                                "\"; using " + std::string(default_provider().id) +
                                ". Known: " + provider_id_list());
        cfg.use_provider(default_provider().id, model_explicit, base_url_explicit);
    }

    // Environment still wins for the model.
    if (const char* m = std::getenv("PPCODE_MODEL"); m && *m) cfg.model = m;

    return cfg;
}

bool Config::save(std::string* error) const {
    try {
        fs::path p(config_path);
        if (p.has_parent_path()) fs::create_directories(p.parent_path());

        json j;
        j["model"]              = model;
        j["provider_id"]        = provider_id;
        j["base_url"]           = base_url;
        j["system_prompt"]      = system_prompt;
        j["temperature"]        = temperature;
        j["max_tokens"]         = max_tokens;
        j["max_turns"]          = max_turns;
        j["referer"]            = referer;
        j["title"]              = title;
        j["unicode"]            = unicode;
        j["color"]              = color;
        j["auto_approve_reads"] = auto_approve_reads;
        j["yolo"]               = yolo;

        json servers = json::array();
        for (const McpServerConfig& m : mcp_servers) {
            json s;
            s["name"]      = m.name;
            s["transport"] = m.transport;
            s["enabled"]   = m.enabled;
            if (!m.command.empty()) s["command"] = m.command;
            if (!m.args.empty())    s["args"]    = m.args;
            if (!m.url.empty())     s["url"]     = m.url;
            if (!m.env.empty())     s["env"]     = m.env;
            if (!m.headers.empty()) s["headers"] = m.headers;
            servers.push_back(s);
        }
        j["mcp_servers"] = servers;

        return write_file_text(config_path, j.dump(2) + "\n", error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::string Config::default_system_prompt() const {
    return
        "You are ppcode, a terminal coding assistant running natively on a PowerPC "
        "Power Mac G5 under Mac OS X Server 10.5.8 (Leopard, Darwin 9.8.0, big-endian).\n"
        "\n"
        "You have tools to read, write, and edit files, list directories, search, and "
        "run shell commands. Use them rather than guessing about file contents. Read a "
        "file before you edit it.\n"
        "\n"
        "Environment notes that matter here:\n"
        "- The modern toolchain is MacPorts under /opt/local. Prefer gcc-mp-15/g++-mp-15 "
        "(GCC 15.2, full C++23) over the system GCC 4.0.1.\n"
        "- If you use clang-mp-3.3, always pass -fPIC; without it PPC codegen emits bad "
        "absolute addresses and binaries fault at runtime. For Objective-C it also needs "
        "-fobjc-runtime=macosx-fragile-10.5.\n"
        "- There is no Node.js on this machine.\n"
        "- The shell is zsh; coreutils are the BSD/Leopard vintage, so avoid GNU-only flags.\n"
        "\n"
        "Be concise. Prefer showing a small diff or the exact command over long prose.";
}

std::string Config::effective_system_prompt() const {
    return system_prompt.empty() ? default_system_prompt() : system_prompt;
}

} // namespace ppcode

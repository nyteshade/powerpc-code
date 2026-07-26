#include "job.hpp"

#include "attach.hpp"
#include "yaml.hpp"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::job {

namespace {

// Every key we understand at the top level. Anything else is a typo worth
// reporting, because a silently ignored key means the job did not do what the
// file says it does.
const char* kKnownKeys[] = {
    "name", "description", "title",
    "model", "models", "fallbacks", "provider", "reasoning", "effort",
    "temperature", "top_p", "max_tokens", "max_turns", "seed",
    "web_search", "web", "web_max_results",
    "cwd", "yolo", "tools", "allow_tools", "deny_tools",
    "environment", "env", "env_detail", "knowledge",
    "system", "system_append",
    "output", "save", "resume", "attachments", "attach",
};

// OpenRouter's provider-preferences keys, per their provider-selection docs.
const char* kProviderKeys[] = {
    "sort", "order", "allow_fallbacks", "only", "ignore", "require_parameters",
    "data_collection", "quantizations", "max_price", "experimental",
};

bool known(const std::string& key, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; i++) if (key == list[i]) return true;
    return false;
}

std::optional<double> opt_num(const json& j, const std::string& key) {
    const json* v = jptr(j, key);
    if (!v || !v->is_number()) return std::nullopt;
    return v->get<double>();
}

std::optional<int> opt_int(const json& j, const std::string& key) {
    const json* v = jptr(j, key);
    if (!v || !v->is_number()) return std::nullopt;
    return static_cast<int>(v->get<int64_t>());
}

std::optional<bool> opt_bool(const json& j, const std::string& key) {
    const json* v = jptr(j, key);
    if (!v || !v->is_boolean()) return std::nullopt;
    return v->get<bool>();
}

std::vector<std::string> str_list(const json& j, const std::string& key) {
    std::vector<std::string> out;
    const json* v = jptr(j, key);
    if (!v) return out;
    if (v->is_string()) {
        out.push_back(v->get<std::string>());
    } else if (v->is_array()) {
        for (const json& s : *v)
            if (s.is_string()) out.push_back(s.get<std::string>());
    }
    return out;
}

void parse_provider(const json& src, Spec* spec, std::vector<std::string>* warnings) {
    if (!src.is_object()) {
        if (warnings) warnings->push_back("'provider' must be a mapping; ignored");
        return;
    }
    json out = json::object();
    for (auto it = src.begin(); it != src.end(); ++it) {
        const std::string& k = it.key();
        if (!known(k, kProviderKeys, sizeof(kProviderKeys) / sizeof(kProviderKeys[0]))) {
            if (warnings)
                warnings->push_back("unknown provider key '" + k +
                                    "'; passing it through anyway");
        }
        // Normalise the two keys where a scalar is the natural thing to write
        // but the API wants a list.
        if ((k == "order" || k == "only" || k == "ignore" || k == "quantizations") &&
            it.value().is_string()) {
            out[k] = json::array({it.value()});
        } else {
            out[k] = it.value();
        }
    }
    if (const json* s = jptr(out, "sort"); s && s->is_string()) {
        std::string v = to_lower(s->get<std::string>());
        if (v != "price" && v != "throughput" && v != "latency") {
            if (warnings)
                warnings->push_back("provider.sort should be price, throughput, or "
                                    "latency (got '" + v + "')");
        }
        out["sort"] = v;
    }
    if (const json* dc = jptr(out, "data_collection"); dc && dc->is_string()) {
        std::string v = to_lower(dc->get<std::string>());
        if (v != "allow" && v != "deny" && warnings)
            warnings->push_back("provider.data_collection should be allow or deny");
        out["data_collection"] = v;
    }
    spec->provider = out;
}

void parse_reasoning(const json& src, Spec* spec, std::vector<std::string>* warnings) {
    json out = json::object();
    if (src.is_string()) {
        // reasoning: high
        out["effort"] = to_lower(src.get<std::string>());
    } else if (src.is_boolean()) {
        out["enabled"] = src.get<bool>();
    } else if (src.is_object()) {
        if (const json* e = jptr(src, "effort"); e && e->is_string())
            out["effort"] = to_lower(e->get<std::string>());
        if (auto mt = opt_int(src, "max_tokens")) out["max_tokens"] = *mt;
        if (auto ex = opt_bool(src, "exclude")) out["exclude"] = *ex;
        if (auto en = opt_bool(src, "enabled")) out["enabled"] = *en;
    } else {
        if (warnings) warnings->push_back("'reasoning' must be a string, bool, or mapping");
        return;
    }
    if (const json* e = jptr(out, "effort"); e && e->is_string()) {
        std::string v = e->get<std::string>();
        if (v != "low" && v != "medium" && v != "high" && v != "minimal" && warnings)
            warnings->push_back("reasoning.effort should be minimal, low, medium, or "
                                "high (got '" + v + "')");
    }
    spec->reasoning = out;
}

void parse_attachments(const json& src, Spec* spec, std::vector<std::string>* warnings) {
    if (src.is_string()) {
        spec->attachments.push_back({src.get<std::string>(), "auto", "auto", ""});
        return;
    }
    if (!src.is_array()) {
        if (warnings) warnings->push_back("'attachments' must be a list");
        return;
    }
    for (const json& a : src) {
        if (a.is_string()) {
            spec->attachments.push_back({a.get<std::string>(), "auto", "auto", ""});
            continue;
        }
        if (!a.is_object()) continue;
        Attachment at;
        at.path = jstr(a, "path");
        if (at.path.empty()) at.path = jstr(a, "url");
        if (at.path.empty()) {
            if (warnings) warnings->push_back("attachment with no path or url; ignored");
            continue;
        }
        at.kind = jstr(a, "kind", "auto");
        at.detail = jstr(a, "detail", "auto");
        at.alt = jstr(a, "alt");
        spec->attachments.push_back(std::move(at));
    }
}

} // namespace

bool Spec::valid(std::string* error) const {
    if (trim(prompt).empty()) {
        if (error)
            *error = "the job body is empty -- put the task description in markdown "
                     "after the closing --- of the frontmatter";
        return false;
    }
    if (!output.empty() && output != "text" && output != "json" &&
        output != "stream-json") {
        if (error) *error = "output must be text, json, or stream-json";
        return false;
    }
    return true;
}

bool parse_text(const std::string& text, Spec* out,
                std::vector<std::string>* warnings, std::string* error) {
    std::string front, body, ferr;
    if (!yaml::split_frontmatter(text, &front, &body, &ferr)) {
        if (error) *error = ferr;
        return false;
    }

    Spec spec;
    spec.prompt = trim(body);

    if (!trim(front).empty()) {
        json meta;
        std::string yerr;
        if (!yaml::parse(front, &meta, &yerr)) {
            if (error) *error = "frontmatter: " + yerr;
            return false;
        }
        if (!meta.is_object()) {
            if (error) *error = "frontmatter must be a mapping of keys to values";
            return false;
        }

        for (auto it = meta.begin(); it != meta.end(); ++it) {
            if (!known(it.key(), kKnownKeys,
                       sizeof(kKnownKeys) / sizeof(kKnownKeys[0])) && warnings)
                warnings->push_back("unknown frontmatter key '" + it.key() +
                                    "'; ignored");
        }

        spec.name = jstr(meta, "name", jstr(meta, "title"));
        spec.description = jstr(meta, "description");

        spec.model = jstr(meta, "model");
        spec.model_fallbacks = str_list(meta, "models");
        if (spec.model_fallbacks.empty())
            spec.model_fallbacks = str_list(meta, "fallbacks");
        // "models: [a, b, c]" with no separate "model" means a is primary.
        if (spec.model.empty() && !spec.model_fallbacks.empty()) {
            spec.model = spec.model_fallbacks.front();
            spec.model_fallbacks.erase(spec.model_fallbacks.begin());
        }

        if (const json* p = jptr(meta, "provider")) parse_provider(*p, &spec, warnings);
        if (const json* r = jptr(meta, "reasoning")) parse_reasoning(*r, &spec, warnings);
        else if (const json* e = jptr(meta, "effort")) parse_reasoning(*e, &spec, warnings);

        spec.temperature = opt_num(meta, "temperature");
        spec.top_p       = opt_num(meta, "top_p");
        spec.max_tokens  = opt_int(meta, "max_tokens");
        spec.max_turns   = opt_int(meta, "max_turns");
        spec.seed        = opt_int(meta, "seed");
        spec.max_cost    = opt_num(meta, "max_cost");
        spec.cache_mode  = to_lower(jstr(meta, "cache_mode"));

        // web_search: true, or web: { max_results: 3 }
        spec.web_search = opt_bool(meta, "web_search");
        spec.web_max_results = opt_int(meta, "web_max_results");
        if (const json* w = jptr(meta, "web")) {
            if (w->is_boolean()) spec.web_search = w->get<bool>();
            else if (w->is_object()) {
                if (auto en = opt_bool(*w, "enabled")) spec.web_search = en;
                else spec.web_search = true;
                if (auto mr = opt_int(*w, "max_results")) spec.web_max_results = mr;
            }
        }

        spec.cwd = jstr(meta, "cwd");
        spec.yolo = opt_bool(meta, "yolo");

        spec.allow_tools = str_list(meta, "allow_tools");
        spec.deny_tools  = str_list(meta, "deny_tools");
        if (const json* t = jptr(meta, "tools")) {
            if (t->is_array() || t->is_string()) {
                // tools: [a, b] is shorthand for the allow list.
                for (const std::string& s : str_list(meta, "tools"))
                    spec.allow_tools.push_back(s);
            } else if (t->is_object()) {
                for (const std::string& s : str_list(*t, "allow"))
                    spec.allow_tools.push_back(s);
                for (const std::string& s : str_list(*t, "deny"))
                    spec.deny_tools.push_back(s);
                if (auto y = opt_bool(*t, "yolo")) spec.yolo = y;
                if (auto y = opt_bool(*t, "all")) spec.yolo = y;
            }
        }

        spec.env_detail = jstr(meta, "env_detail");
        spec.knowledge = opt_bool(meta, "knowledge");
        for (const char* key : {"environment", "env"}) {
            const json* e = jptr(meta, key);
            if (!e) continue;
            if (e->is_string()) {
                spec.env_detail = e->get<std::string>();
            } else if (e->is_boolean()) {
                if (!e->get<bool>()) spec.env_detail = "none";
            } else if (e->is_object()) {
                if (std::string d = jstr(*e, "detail"); !d.empty()) spec.env_detail = d;
                if (auto k = opt_bool(*e, "knowledge")) spec.knowledge = k;
            }
        }

        spec.system        = jstr(meta, "system");
        spec.system_append = jstr(meta, "system_append");
        spec.output        = to_lower(jstr(meta, "output"));
        spec.save          = jstr(meta, "save");
        spec.resume        = jstr(meta, "resume");

        if (const json* a = jptr(meta, "attachments")) parse_attachments(*a, &spec, warnings);
        else if (const json* a2 = jptr(meta, "attach")) parse_attachments(*a2, &spec, warnings);
    }

    std::string verr;
    if (!spec.valid(&verr)) {
        if (error) *error = verr;
        return false;
    }
    *out = std::move(spec);
    return true;
}

bool parse_file(const std::string& path, Spec* out,
                std::vector<std::string>* warnings, std::string* error) {
    std::string full = expand_user(path);
    std::string text;
    if (!read_file_text(full, &text, error)) return false;
    if (!parse_text(text, out, warnings, error)) {
        if (error) *error = full + ": " + *error;
        return false;
    }
    out->source_path = full;
    if (out->name.empty()) out->name = fs::path(full).stem().string();
    return true;
}

void apply_to_config(const Spec& spec, Config* cfg) {
    if (!spec.model.empty())            cfg->model = spec.model;
    if (!spec.model_fallbacks.empty())  cfg->model_fallbacks = spec.model_fallbacks;
    if (spec.provider.is_object() && !spec.provider.empty())
        cfg->provider = spec.provider;
    if (spec.reasoning.is_object() && !spec.reasoning.empty())
        cfg->reasoning = spec.reasoning;

    if (spec.temperature) cfg->temperature = *spec.temperature;
    if (spec.top_p)       cfg->top_p = *spec.top_p;
    if (spec.max_tokens)  cfg->max_tokens = *spec.max_tokens;
    if (spec.max_turns)   cfg->max_turns = *spec.max_turns;
    if (spec.seed)        cfg->seed = *spec.seed;
    if (spec.yolo)        cfg->yolo = *spec.yolo;

    if (spec.web_search)      cfg->web_search = *spec.web_search;
    if (spec.web_max_results) cfg->web_max_results = *spec.web_max_results;
    if (spec.max_cost)        cfg->max_cost = *spec.max_cost;
    if (!spec.cache_mode.empty()) cfg->cache_mode = spec.cache_mode;

    if (!spec.system.empty()) cfg->system_prompt = spec.system;
}

Message build_user_message(const Spec& spec, bool model_supports_images,
                           std::vector<std::string>* warnings) {
    // With nothing attached, a plain string message is what every provider
    // handles best, so do not wrap it in parts unnecessarily.
    if (spec.attachments.empty()) return Message::user(spec.prompt);

    Message m;
    m.role = "user";

    std::vector<ContentPart> parts;
    parts.push_back(ContentPart::make_text(spec.prompt));

    for (const Attachment& a : spec.attachments) {
        attach::Loaded l = attach::load(a.path, a.kind, a.detail,
                                        model_supports_images, spec.cwd);
        if (!l.ok) {
            if (warnings) warnings->push_back("attachment " + a.path + ": " + l.error);
            // Tell the model too, so it does not silently assume it saw the file.
            parts.push_back(ContentPart::make_text(
                "[attachment " + a.path + " could not be loaded: " + l.error + "]"));
            continue;
        }
        if (!l.note.empty() && warnings) warnings->push_back(l.note);
        if (!a.alt.empty())
            parts.push_back(ContentPart::make_text("[" + a.alt + "]"));
        parts.push_back(std::move(l.part));
    }

    m.parts = std::move(parts);
    return m;
}

} // namespace ppcode::job

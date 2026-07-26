#include "xcodeproj.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::xcode {

// ---------------------------------------------------------------------------
// File types
// ---------------------------------------------------------------------------

std::string file_type_for(const std::string& path) {
    std::string p = to_lower(path);
    size_t dot = p.rfind('.');
    std::string ext = (dot == std::string::npos) ? "" : p.substr(dot + 1);

    if (ext == "m")     return "sourcecode.c.objc";
    if (ext == "mm")    return "sourcecode.cpp.objcpp";
    if (ext == "c")     return "sourcecode.c.c";
    if (ext == "cpp" || ext == "cc" || ext == "cxx") return "sourcecode.cpp.cpp";
    if (ext == "h")     return "sourcecode.c.h";
    if (ext == "hpp" || ext == "hh") return "sourcecode.cpp.h";
    if (ext == "s")     return "sourcecode.asm";
    if (ext == "pch")   return "sourcecode.c.h";
    if (ext == "plist") return "text.plist.xml";
    if (ext == "strings") return "text.plist.strings";
    if (ext == "xib")   return "file.xib";
    if (ext == "nib")   return "wrapper.nib";
    if (ext == "png")   return "image.png";
    if (ext == "jpg" || ext == "jpeg") return "image.jpeg";
    if (ext == "tiff" || ext == "tif") return "image.tiff";
    if (ext == "icns")  return "image.icns";
    if (ext == "rtf")   return "text.rtf";
    if (ext == "html")  return "text.html";
    if (ext == "txt")   return "text";
    if (ext == "sh")    return "text.script.sh";
    if (ext == "framework") return "wrapper.framework";
    if (ext == "dylib") return "compiled.mach-o.dylib";
    if (ext == "a")     return "archive.ar";
    return "text";
}

bool is_source_extension(const std::string& path) {
    std::string t = file_type_for(path);
    return starts_with(t, "sourcecode.") && !ends_with(t, ".h");
}

namespace {

// Resources go into the copy phase; headers into neither.
bool is_resource_extension(const std::string& path) {
    std::string t = file_type_for(path);
    return starts_with(t, "image.") || t == "file.xib" || t == "wrapper.nib" ||
           t == "text.plist.strings" || t == "text.rtf" || t == "text.html";
}

bool is_header(const std::string& path) {
    return ends_with(file_type_for(path), ".h");
}

} // namespace

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------

bool Project::load(const std::string& path, std::string* error) {
    std::string p = expand_user(path);
    std::error_code ec;

    if (fs::is_directory(p, ec) && ends_with(p, ".xcodeproj")) {
        p += "/project.pbxproj";
    } else if (fs::is_directory(p, ec)) {
        // A plain directory: look for a single .xcodeproj inside it.
        std::vector<std::string> found;
        for (const auto& e : fs::directory_iterator(p, ec))
            if (ends_with(e.path().string(), ".xcodeproj")) found.push_back(e.path().string());
        if (found.empty()) {
            if (error) *error = "no .xcodeproj found in " + path;
            return false;
        }
        if (found.size() > 1) {
            if (error)
                *error = "several projects in " + path + "; name one explicitly";
            return false;
        }
        p = found[0] + "/project.pbxproj";
    }

    if (!fs::exists(p, ec)) {
        if (error) *error = "not found: " + p;
        return false;
    }

    std::string text;
    if (!read_file_text(p, &text, error)) return false;

    std::string perr;
    root_ = plist::parse(text, &perr);
    if (!root_) {
        if (error) *error = "could not parse " + p + ": " + perr;
        return false;
    }
    objects_ = root_->get("objects");
    if (!objects_ || !objects_->is_dict()) {
        if (error) *error = p + " has no objects dictionary; is it a project file?";
        return false;
    }
    pbxproj_path_ = p;
    return true;
}

bool Project::save(std::string* error) const {
    if (!root_ || pbxproj_path_.empty()) {
        if (error) *error = "no project loaded";
        return false;
    }
    // Keep a one-shot backup: a malformed pbxproj makes a project unopenable,
    // and this is cheap insurance.
    std::string existing;
    if (read_file_text(pbxproj_path_, &existing, nullptr))
        write_file_text(pbxproj_path_ + ".ppcode-bak", existing, nullptr);

    std::string out = plist::serialize(root_, true);

    // Refuse to write something we cannot read back.
    std::string perr;
    if (!plist::parse(out, &perr)) {
        if (error)
            *error = "internal error: the regenerated project does not re-parse (" +
                     perr + "); nothing was written";
        return false;
    }
    return write_file_text(pbxproj_path_, out, error);
}

std::string Project::object_version() const {
    return root_ ? root_->get_string("objectVersion") : "";
}

plist::ValuePtr Project::obj(const std::string& id) const {
    if (!objects_) return nullptr;
    return objects_->get(id);
}

plist::ValuePtr Project::project_object() const {
    if (!root_) return nullptr;
    return obj(root_->get_string("rootObject"));
}

std::string Project::compatibility_version() const {
    plist::ValuePtr p = project_object();
    return p ? p->get_string("compatibilityVersion") : "";
}

std::string Project::new_id() const {
    // Xcode ids are 24 uppercase hex characters. Uniqueness within the file is
    // all that matters, so read real entropy and check for collisions.
    static const char* hex = "0123456789ABCDEF";
    for (int attempt = 0; attempt < 64; attempt++) {
        unsigned char buf[12] = {0};
        if (FILE* f = std::fopen("/dev/urandom", "rb")) {
            size_t n = std::fread(buf, 1, sizeof(buf), f);
            std::fclose(f);
            if (n != sizeof(buf)) {
                // Fall through to the mix below rather than emitting zeros.
            }
        }
        // Mix in the attempt and the current object count so that a urandom
        // failure still yields distinct ids.
        buf[0] ^= static_cast<unsigned char>(attempt);
        buf[1] ^= static_cast<unsigned char>(objects_ ? objects_->entries.size() : 0);

        std::string id;
        for (unsigned char c : buf) {
            id += hex[(c >> 4) & 0xF];
            id += hex[c & 0xF];
        }
        if (!obj(id)) return id;
    }
    return "";
}

std::string Project::find_target(const std::string& name) const {
    plist::ValuePtr proj = project_object();
    if (!proj) return "";
    plist::ValuePtr targets = proj->get("targets");
    if (!targets || !targets->is_array()) return "";
    for (const plist::ValuePtr& t : targets->items) {
        if (!t->is_string()) continue;
        plist::ValuePtr to = obj(t->str);
        if (!to) continue;
        if (name.empty() || to->get_string("name") == name) return t->str;
    }
    return "";
}

std::string Project::phase_of_target(const std::string& target_id,
                                     const std::string& isa) const {
    plist::ValuePtr t = obj(target_id);
    if (!t) return "";
    plist::ValuePtr phases = t->get("buildPhases");
    if (!phases || !phases->is_array()) return "";
    for (const plist::ValuePtr& p : phases->items) {
        if (!p->is_string()) continue;
        plist::ValuePtr po = obj(p->str);
        if (po && po->get_string("isa") == isa) return p->str;
    }
    return "";
}

std::vector<std::string> Project::config_ids(const std::string& list_id,
                                             const std::string& config_name) const {
    std::vector<std::string> out;
    plist::ValuePtr list = obj(list_id);
    if (!list) return out;
    plist::ValuePtr configs = list->get("buildConfigurations");
    if (!configs || !configs->is_array()) return out;
    for (const plist::ValuePtr& c : configs->items) {
        if (!c->is_string()) continue;
        plist::ValuePtr co = obj(c->str);
        if (!co) continue;
        if (config_name.empty() || co->get_string("name") == config_name)
            out.push_back(c->str);
    }
    return out;
}

std::string Project::main_group() const {
    plist::ValuePtr proj = project_object();
    return proj ? proj->get_string("mainGroup") : "";
}

std::string Project::find_group(const std::string& name) const {
    if (name.empty()) return main_group();
    for (const auto& [id, o] : objects_->entries) {
        if (!o->is_dict()) continue;
        if (o->get_string("isa") != "PBXGroup") continue;
        if (o->get_string("name") == name || o->get_string("path") == name)
            return id;
    }
    return "";
}

std::vector<std::string> Project::files_in_phase(const std::string& phase_id) const {
    std::vector<std::string> out;
    plist::ValuePtr phase = obj(phase_id);
    if (!phase) return out;
    plist::ValuePtr files = phase->get("files");
    if (!files || !files->is_array()) return out;
    for (const plist::ValuePtr& f : files->items) {
        if (!f->is_string()) continue;
        plist::ValuePtr bf = obj(f->str);
        if (!bf) continue;
        plist::ValuePtr ref = obj(bf->get_string("fileRef"));
        if (!ref) continue;
        std::string name = ref->get_string("path");
        if (name.empty()) name = ref->get_string("name");
        if (!name.empty()) out.push_back(name);
    }
    return out;
}

std::vector<BuildConfig> Project::project_configs() const {
    std::vector<BuildConfig> out;
    plist::ValuePtr proj = project_object();
    if (!proj) return out;
    for (const std::string& cid :
         config_ids(proj->get_string("buildConfigurationList"), "")) {
        plist::ValuePtr co = obj(cid);
        if (!co) continue;
        BuildConfig bc;
        bc.id = cid;
        bc.name = co->get_string("name");
        if (plist::ValuePtr s = co->get("buildSettings"); s && s->is_dict()) {
            for (const auto& [k, v] : s->entries) {
                if (v->is_string()) bc.settings.emplace_back(k, v->str);
                else if (v->is_array()) {
                    std::string joined;
                    for (const plist::ValuePtr& i : v->items)
                        if (i->is_string()) joined += (joined.empty() ? "" : " ") + i->str;
                    bc.settings.emplace_back(k, joined);
                }
            }
        }
        out.push_back(std::move(bc));
    }
    return out;
}

std::vector<Target> Project::targets() const {
    std::vector<Target> out;
    plist::ValuePtr proj = project_object();
    if (!proj) return out;
    plist::ValuePtr tlist = proj->get("targets");
    if (!tlist || !tlist->is_array()) return out;

    for (const plist::ValuePtr& t : tlist->items) {
        if (!t->is_string()) continue;
        plist::ValuePtr to = obj(t->str);
        if (!to) continue;

        Target tg;
        tg.id = t->str;
        tg.name = to->get_string("name");
        tg.type = to->get_string("productType");
        tg.product_name = to->get_string("productName");

        tg.source_files = files_in_phase(phase_of_target(tg.id, "PBXSourcesBuildPhase"));
        tg.resource_files =
            files_in_phase(phase_of_target(tg.id, "PBXResourcesBuildPhase"));
        tg.frameworks =
            files_in_phase(phase_of_target(tg.id, "PBXFrameworksBuildPhase"));

        for (const std::string& cid :
             config_ids(to->get_string("buildConfigurationList"), "")) {
            plist::ValuePtr co = obj(cid);
            if (!co) continue;
            BuildConfig bc;
            bc.id = cid;
            bc.name = co->get_string("name");
            if (plist::ValuePtr s = co->get("buildSettings"); s && s->is_dict())
                for (const auto& [k, v] : s->entries)
                    if (v->is_string()) bc.settings.emplace_back(k, v->str);
            tg.configs.push_back(std::move(bc));
        }
        out.push_back(std::move(tg));
    }
    return out;
}

std::string Project::describe() const {
    std::string out = pbxproj_path_ + "\n";
    out += "objectVersion " + object_version();
    if (std::string cv = compatibility_version(); !cv.empty())
        out += ", compatibility " + cv;
    out += "\n";

    std::vector<BuildConfig> pc = project_configs();
    if (!pc.empty()) {
        out += "\nProject-level configurations:\n";
        for (const BuildConfig& c : pc) {
            out += "  " + c.name + "\n";
            for (const auto& [k, v] : c.settings)
                out += "      " + k + " = " + elide(v, 90) + "\n";
        }
    }

    for (const Target& t : targets()) {
        out += "\nTarget: " + t.name;
        if (!t.type.empty()) out += "  (" + t.type + ")\n";
        else out += "\n";

        if (!t.source_files.empty())
            out += "  sources:   " + join(t.source_files, ", ") + "\n";
        if (!t.resource_files.empty())
            out += "  resources: " + join(t.resource_files, ", ") + "\n";
        if (!t.frameworks.empty())
            out += "  linked:    " + join(t.frameworks, ", ") + "\n";

        for (const BuildConfig& c : t.configs) {
            out += "  configuration " + c.name + ":\n";
            for (const auto& [k, v] : c.settings)
                out += "      " + k + " = " + elide(v, 90) + "\n";
        }
    }
    return out;
}

bool Project::add_file(const std::string& file_path, const std::string& target_name,
                       const std::string& group_name, std::string* error) {
    if (!objects_) { if (error) *error = "no project loaded"; return false; }

    std::string rel = file_path;
    std::string base = fs::path(rel).filename().string();

    // Refuse a duplicate rather than creating a second reference to the same
    // path, which Xcode shows as two entries and compiles twice.
    for (const auto& [id, o] : objects_->entries) {
        if (!o->is_dict()) continue;
        if (o->get_string("isa") != "PBXFileReference") continue;
        if (o->get_string("path") == rel) {
            if (error) *error = rel + " is already in the project";
            return false;
        }
    }

    std::string group_id = find_group(group_name);
    if (group_id.empty()) {
        if (error)
            *error = group_name.empty() ? "project has no main group"
                                        : "no group named " + group_name;
        return false;
    }

    // 1. The file reference.
    std::string ref_id = new_id();
    if (ref_id.empty()) { if (error) *error = "could not allocate an id"; return false; }
    auto ref = plist::Value::make_dict();
    ref->comment = base;
    ref->set("isa", plist::Value::make_string("PBXFileReference"));
    ref->set("fileEncoding", plist::Value::make_string("4"));
    ref->set("lastKnownFileType", plist::Value::make_string(file_type_for(rel)));
    ref->set("path", plist::Value::make_string(rel));
    ref->set("sourceTree", plist::Value::make_string("<group>"));
    objects_->set(ref_id, ref);

    // 2. Into the group so it shows up in the navigator.
    plist::ValuePtr group = obj(group_id);
    plist::ValuePtr children = group ? group->get("children") : nullptr;
    if (!children || !children->is_array()) {
        if (error) *error = "group has no children array";
        return false;
    }
    auto child = plist::Value::make_string(ref_id);
    child->comment = base;
    children->items.push_back(child);

    // 3. Into a build phase, when a target was named and the file is built.
    if (!target_name.empty() || is_source_extension(rel) || is_resource_extension(rel)) {
        std::string target_id = find_target(target_name);
        if (target_id.empty()) {
            if (error)
                *error = target_name.empty() ? "project has no targets"
                                             : "no target named " + target_name;
            return false;
        }
        if (is_header(rel)) {
            // Headers belong in the project but not in a phase.
            return true;
        }
        std::string isa = is_resource_extension(rel) ? "PBXResourcesBuildPhase"
                                                     : "PBXSourcesBuildPhase";
        std::string phase_id = phase_of_target(target_id, isa);
        if (phase_id.empty()) {
            if (error)
                *error = "target has no " + isa +
                         "; add the file manually or pick another target";
            return false;
        }

        std::string bf_id = new_id();
        auto bf = plist::Value::make_dict();
        bf->comment = base + " in " +
                      (isa == "PBXResourcesBuildPhase" ? "Resources" : "Sources");
        bf->set("isa", plist::Value::make_string("PBXBuildFile"));
        auto fref = plist::Value::make_string(ref_id);
        fref->comment = base;
        bf->set("fileRef", fref);
        objects_->set(bf_id, bf);

        plist::ValuePtr phase = obj(phase_id);
        plist::ValuePtr files = phase->get("files");
        if (!files || !files->is_array()) {
            if (error) *error = "build phase has no files array";
            return false;
        }
        auto entry = plist::Value::make_string(bf_id);
        entry->comment = bf->comment;
        files->items.push_back(entry);
    }
    return true;
}

bool Project::set_setting(const std::string& key, const std::string& value,
                          const std::string& target_name,
                          const std::string& config_name, bool remove,
                          std::string* error) {
    if (!objects_) { if (error) *error = "no project loaded"; return false; }

    std::string list_id;
    if (target_name.empty()) {
        plist::ValuePtr proj = project_object();
        if (!proj) { if (error) *error = "no project object"; return false; }
        list_id = proj->get_string("buildConfigurationList");
    } else {
        std::string tid = find_target(target_name);
        if (tid.empty()) {
            if (error) *error = "no target named " + target_name;
            return false;
        }
        list_id = obj(tid)->get_string("buildConfigurationList");
    }

    std::vector<std::string> ids = config_ids(list_id, config_name);
    if (ids.empty()) {
        if (error)
            *error = config_name.empty() ? "no build configurations found"
                                         : "no configuration named " + config_name;
        return false;
    }

    for (const std::string& cid : ids) {
        plist::ValuePtr co = obj(cid);
        if (!co) continue;
        plist::ValuePtr settings = co->get("buildSettings");
        if (!settings || !settings->is_dict()) {
            settings = plist::Value::make_dict();
            co->set("buildSettings", settings);
        }
        if (remove) settings->erase(key);
        else settings->set(key, plist::Value::make_string(value));
    }
    return true;
}

bool Project::add_framework(const std::string& framework,
                            const std::string& target_name, std::string* error) {
    if (!objects_) { if (error) *error = "no project loaded"; return false; }

    std::string name = framework;
    if (!ends_with(name, ".framework")) name += ".framework";
    std::string path = "/System/Library/Frameworks/" + name;

    std::string target_id = find_target(target_name);
    if (target_id.empty()) {
        if (error)
            *error = target_name.empty() ? "project has no targets"
                                         : "no target named " + target_name;
        return false;
    }
    std::string phase_id = phase_of_target(target_id, "PBXFrameworksBuildPhase");
    if (phase_id.empty()) {
        if (error) *error = "target has no frameworks build phase";
        return false;
    }

    // Reuse an existing reference if the framework is already known.
    std::string ref_id;
    for (const auto& [id, o] : objects_->entries) {
        if (!o->is_dict()) continue;
        if (o->get_string("isa") != "PBXFileReference") continue;
        if (o->get_string("path") == path || o->get_string("name") == name) {
            ref_id = id;
            break;
        }
    }
    if (ref_id.empty()) {
        ref_id = new_id();
        auto ref = plist::Value::make_dict();
        ref->comment = name;
        ref->set("isa", plist::Value::make_string("PBXFileReference"));
        ref->set("lastKnownFileType", plist::Value::make_string("wrapper.framework"));
        ref->set("name", plist::Value::make_string(name));
        ref->set("path", plist::Value::make_string(path));
        ref->set("sourceTree", plist::Value::make_string("<absolute>"));
        objects_->set(ref_id, ref);

        if (std::string g = find_group("Linked Frameworks"); !g.empty()) {
            plist::ValuePtr children = obj(g)->get("children");
            if (children && children->is_array()) {
                auto c = plist::Value::make_string(ref_id);
                c->comment = name;
                children->items.push_back(c);
            }
        }
    }

    // Already linked?
    for (const std::string& existing :
         files_in_phase(phase_of_target(target_id, "PBXFrameworksBuildPhase"))) {
        if (existing == path || existing == name) {
            if (error) *error = name + " is already linked into that target";
            return false;
        }
    }

    std::string bf_id = new_id();
    auto bf = plist::Value::make_dict();
    bf->comment = name + " in Frameworks";
    bf->set("isa", plist::Value::make_string("PBXBuildFile"));
    auto fref = plist::Value::make_string(ref_id);
    fref->comment = name;
    bf->set("fileRef", fref);
    objects_->set(bf_id, bf);

    plist::ValuePtr files = obj(phase_id)->get("files");
    if (!files || !files->is_array()) {
        if (error) *error = "frameworks phase has no files array";
        return false;
    }
    auto entry = plist::Value::make_string(bf_id);
    entry->comment = bf->comment;
    files->items.push_back(entry);
    return true;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

namespace {

bool open_project(const json& a, ToolContext& ctx, Project* p, std::string* err) {
    std::string path = jstr(a, "project");
    if (path.empty()) path = ".";
    return p->load(resolve_path(path, ctx.cwd), err);
}

} // namespace

void add_tools(ToolRegistry& registry) {
    {
        Tool t;
        t.spec.name = "xcode_info";
        t.spec.description =
            "Inspect an Xcode project: targets, product types, source and resource "
            "files, linked frameworks, and every build setting per configuration. "
            "Use this before changing a project so you know what is there.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "project": {"type": "string", "description": "Path to a .xcodeproj, its project.pbxproj, or a directory containing one. Defaults to the working directory."}
            }
        })");
        t.kind = ToolKind::Read;
        t.source = "builtin";
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            Project p;
            std::string err;
            if (!open_project(a, ctx, &p, &err)) return ToolResult::err(err);
            return ToolResult::ok(p.describe());
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "xcode_add_file";
        t.spec.description =
            "Add an existing file to an Xcode project. Source files go into the "
            "target's compile phase, resources into the copy-resources phase, and "
            "headers are added to the project only. The file must already exist on "
            "disk -- write it first.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "project": {"type": "string", "description": "Project path. Defaults to the working directory."},
                "path":    {"type": "string", "description": "File path as it should appear in the project, relative to the project directory."},
                "target":  {"type": "string", "description": "Target name. Defaults to the first target."},
                "group":   {"type": "string", "description": "Navigator group to add it to. Defaults to the main group."}
            },
            "required": ["path"]
        })");
        t.kind = ToolKind::Mutate;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"xcode_add_file  " + jstr(a, "path"),
                               "project " + jstr(a, "project", ".") +
                                   ", target " + jstr(a, "target", "(first)")};
        };
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            std::string path = jstr(a, "path");
            if (path.empty()) return ToolResult::err("'path' is required");
            Project p;
            std::string err;
            if (!open_project(a, ctx, &p, &err)) return ToolResult::err(err);
            if (!p.add_file(path, jstr(a, "target"), jstr(a, "group"), &err))
                return ToolResult::err(err);
            if (!p.save(&err)) return ToolResult::err(err);
            return ToolResult::ok("Added " + path + " to " + p.pbxproj_path() +
                                  " (a backup was written alongside it)");
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "xcode_set_setting";
        t.spec.description =
            "Set or remove a build setting in an Xcode project, either at the "
            "project level or on a target, for one configuration or all of them. "
            "Examples: GCC_VERSION, ARCHS, MACOSX_DEPLOYMENT_TARGET, SDKROOT, "
            "OTHER_CFLAGS, GCC_OPTIMIZATION_LEVEL.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "project": {"type": "string",  "description": "Project path. Defaults to the working directory."},
                "key":     {"type": "string",  "description": "Build setting name."},
                "value":   {"type": "string",  "description": "Value to set. Ignored when remove is true."},
                "target":  {"type": "string",  "description": "Target name. Omit for the project level."},
                "config":  {"type": "string",  "description": "Configuration name such as Debug or Release. Omit for all."},
                "remove":  {"type": "boolean", "description": "Remove the setting instead of setting it."}
            },
            "required": ["key"]
        })");
        t.kind = ToolKind::Mutate;
        t.source = "builtin";
        t.preview = [](const json& a) {
            bool rm = jbool(a, "remove", false);
            return ToolPreview{"xcode_set_setting  " + jstr(a, "key"),
                               rm ? "remove" : "= " + jstr(a, "value")};
        };
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            std::string key = jstr(a, "key");
            if (key.empty()) return ToolResult::err("'key' is required");
            bool remove = jbool(a, "remove", false);
            std::string value = jstr(a, "value");
            if (!remove && value.empty())
                return ToolResult::err("'value' is required unless remove is true");

            Project p;
            std::string err;
            if (!open_project(a, ctx, &p, &err)) return ToolResult::err(err);
            if (!p.set_setting(key, value, jstr(a, "target"), jstr(a, "config"),
                               remove, &err))
                return ToolResult::err(err);
            if (!p.save(&err)) return ToolResult::err(err);
            return ToolResult::ok((remove ? "Removed " : "Set ") + key +
                                  (remove ? "" : " = " + value) + " in " +
                                  p.pbxproj_path());
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "xcode_add_framework";
        t.spec.description =
            "Link a system framework into an Xcode target, e.g. WebKit, "
            "QuartzCore, Carbon, CoreData.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "project":   {"type": "string", "description": "Project path. Defaults to the working directory."},
                "framework": {"type": "string", "description": "Framework name, with or without the .framework suffix."},
                "target":    {"type": "string", "description": "Target name. Defaults to the first target."}
            },
            "required": ["framework"]
        })");
        t.kind = ToolKind::Mutate;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"xcode_add_framework  " + jstr(a, "framework"),
                               "target " + jstr(a, "target", "(first)")};
        };
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            std::string fw = jstr(a, "framework");
            if (fw.empty()) return ToolResult::err("'framework' is required");
            Project p;
            std::string err;
            if (!open_project(a, ctx, &p, &err)) return ToolResult::err(err);
            if (!p.add_framework(fw, jstr(a, "target"), &err))
                return ToolResult::err(err);
            if (!p.save(&err)) return ToolResult::err(err);
            return ToolResult::ok("Linked " + fw + " into " + p.pbxproj_path());
        };
        registry.add(std::move(t));
    }
}

} // namespace ppcode::xcode

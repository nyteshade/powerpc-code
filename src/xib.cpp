#include "xib.hpp"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::xib {

namespace {

// A child element carrying this `key` attribute. Free function so the parsing
// helpers below can use it without a Document.
xml::NodePtr keyed_child(const xml::NodePtr& parent, const std::string& key) {
    if (!parent) return nullptr;
    for (const xml::NodePtr& c : parent->children)
        if (c->is_element() && c->attr("key") == key) return c;
    return nullptr;
}

// Build an NSMutableDictionary in this archive's parallel-array encoding.
xml::NodePtr make_dict(const std::string& key,
                       const std::vector<std::pair<std::string, std::string>>& kv) {
    auto dict = xml::Node::element("object");
    dict->set_attr("class", "NSMutableDictionary");
    dict->set_attr("key", key);

    auto flag = xml::Node::element("bool");
    flag->set_attr("key", "EncodedWithXMLCoder");
    flag->set_inner_text("YES");
    dict->append(flag);

    auto keys = xml::Node::element("object");
    keys->set_attr("class", "NSMutableArray");
    keys->set_attr("key", "dict.sortedKeys");
    auto kflag = xml::Node::element("bool");
    kflag->set_attr("key", "EncodedWithXMLCoder");
    kflag->set_inner_text("YES");
    keys->append(kflag);

    auto vals = xml::Node::element("object");
    vals->set_attr("class", "NSMutableArray");
    vals->set_attr("key", "dict.values");
    auto vflag = xml::Node::element("bool");
    vflag->set_attr("key", "EncodedWithXMLCoder");
    vflag->set_inner_text("YES");
    vals->append(vflag);

    for (const auto& [k, v] : kv) {
        auto ks = xml::Node::element("string");
        ks->set_inner_text(k);
        keys->append(ks);
        auto vs = xml::Node::element("string");
        vs->set_inner_text(v);
        vals->append(vs);
    }
    dict->append(keys);
    dict->append(vals);
    return dict;
}

// A short label for an object: whatever key identifies it in the inspector.
std::string label_for(const xml::NodePtr& obj) {
    static const char* keys[] = {"NSTitle", "NSClassName", "NSWindowTitle",
                                 "NSContents", "IBUIText", "NSFrame",
                                 "NSFrameSize", "NSKeyEquiv"};
    for (const char* k : keys) {
        for (const xml::NodePtr& c : obj->children) {
            if (!c->is_element()) continue;
            if (c->attr("key") != k) continue;
            std::string v = trim(c->inner_text());
            if (!v.empty()) return std::string(k) + "=" + elide(v, 46);
        }
    }
    return "";
}

void walk(const xml::NodePtr& node, int depth, std::vector<ObjectNode>* out) {
    for (const xml::NodePtr& c : node->children) {
        if (!c->is_element()) continue;
        if (c->name == "object") {
            ObjectNode on;
            on.id = c->attr("id");
            on.cls = c->attr("class");
            on.title = label_for(c);
            on.depth = depth;
            out->push_back(std::move(on));
            walk(c, depth + 1, out);
        } else {
            // Arrays and dictionaries are containers; descend without counting
            // them as objects in their own right.
            walk(c, depth, out);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------

bool Document::load(const std::string& path, std::string* error) {
    std::string p = expand_user(path);
    std::error_code ec;

    // A .nib on 10.5 may be a directory wrapper containing keyedobjects.nib
    // (binary) -- that is compiled output and cannot be edited here.
    if (ends_with(to_lower(p), ".nib")) {
        if (error)
            *error = p + " is a compiled nib. Edit the .xib source instead; a "
                         ".nib is the build product and is binary.";
        return false;
    }
    if (!fs::exists(p, ec)) {
        if (error) *error = "not found: " + p;
        return false;
    }

    std::string text;
    if (!read_file_text(p, &text, error)) return false;

    std::string perr;
    if (!xml::parse(text, &doc_, &perr)) {
        if (error) *error = "could not parse " + p + ": " + perr;
        return false;
    }
    if (!doc_.root || doc_.root->name != "archive") {
        if (error) *error = p + " is not an Interface Builder archive";
        return false;
    }
    path_ = p;
    return true;
}

bool Document::save(std::string* error) const {
    if (path_.empty() || !doc_.root) {
        if (error) *error = "no document loaded";
        return false;
    }
    // A nib that Interface Builder cannot reopen is worse than no edit, so keep
    // a backup and refuse to write anything that does not re-parse.
    std::string existing;
    if (read_file_text(path_, &existing, nullptr))
        write_file_text(path_ + ".ppcode-bak", existing, nullptr);

    std::string out = xml::serialize(doc_, true);
    xml::Document check;
    std::string perr;
    if (!xml::parse(out, &check, &perr)) {
        if (error)
            *error = "internal error: the regenerated nib does not re-parse (" +
                     perr + "); nothing was written";
        return false;
    }
    return write_file_text(path_, out, error);
}

xml::NodePtr Document::data() const {
    return doc_.root ? doc_.root->first_child("data") : nullptr;
}

// Interface Builder keys everything by a `key` attribute rather than by tag.
xml::NodePtr Document::keyed(const xml::NodePtr& parent,
                             const std::string& key) const {
    return keyed_child(parent, key);
}

xml::NodePtr Document::root_objects() const {
    return keyed(data(), "IBDocument.RootObjects");
}

xml::NodePtr Document::flattened_objects() const {
    return keyed(data(), "IBDocument.Objects");
}

std::string Document::format_version() const {
    return doc_.root ? doc_.root->attr("version") : "";
}

std::string Document::system_target() const {
    xml::NodePtr t = keyed(data(), "IBDocument.SystemTarget");
    return t ? t->inner_text() : "";
}



std::vector<ObjectNode> Document::objects() const {
    std::vector<ObjectNode> out;
    xml::NodePtr ro = root_objects();
    if (ro) walk(ro, 0, &out);
    return out;
}

std::vector<Connection> Document::connections() const {
    std::vector<Connection> out;
    xml::NodePtr flat = flattened_objects();
    if (!flat) return out;

    // Map id -> class, so a connection can name the classes it joins.
    std::map<std::string, std::string> class_of;
    for (const ObjectNode& o : objects())
        if (!o.id.empty()) class_of[o.id] = o.cls;

    xml::NodePtr records = keyed(flat, "connectionRecords");
    if (!records) return out;

    for (const xml::NodePtr& rec : records->find_all("object")) {
        if (rec->attr("class") != "IBConnectionRecord") continue;
        xml::NodePtr conn = keyed(rec, "connection");
        if (!conn) continue;

        Connection c;
        std::string cls = conn->attr("class");
        if (cls == "IBOutletConnection")      c.kind = "outlet";
        else if (cls == "IBActionConnection") c.kind = "action";
        else if (cls.find("Binding") != std::string::npos) c.kind = "binding";
        else c.kind = cls;

        for (const xml::NodePtr& f : conn->children) {
            if (!f->is_element()) continue;
            std::string key = f->attr("key");
            if (key == "label") c.label = f->inner_text();
            else if (key == "source") c.source = f->attr("ref");
            else if (key == "destination") c.destination = f->attr("ref");
        }
        if (auto it = class_of.find(c.source); it != class_of.end())
            c.source_class = it->second;
        if (auto it = class_of.find(c.destination); it != class_of.end())
            c.dest_class = it->second;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<ClassDescription> Document::classes() const {
    std::vector<ClassDescription> out;
    xml::NodePtr classes = keyed(data(), "IBDocument.Classes");
    if (!classes) return out;

    for (const xml::NodePtr& pcd : classes->find_all("object")) {
        if (pcd->attr("class") != "IBPartialClassDescription") continue;

        ClassDescription d;
        for (const xml::NodePtr& f : pcd->children) {
            if (!f->is_element()) continue;
            std::string key = f->attr("key");
            if (key == "className")      d.name = f->inner_text();
            else if (key == "superclassName") d.superclass = f->inner_text();
            else if (key == "sourceIdentifier") {
                xml::NodePtr rel = keyed(f, "relativePath");
                if (rel) d.source_file = rel->inner_text();
            } else if (key == "outlets" || key == "actions") {
                // This XML coder encodes an NSMutableDictionary as two parallel
                // arrays -- dict.sortedKeys and dict.values -- rather than as
                // keyed children. Read them in step.
                std::vector<std::string> names, types;
                if (xml::NodePtr k = keyed_child(f, "dict.sortedKeys"))
                    for (const xml::NodePtr& s : k->find_children("string"))
                        names.push_back(s->inner_text());
                if (xml::NodePtr v = keyed_child(f, "dict.values"))
                    for (const xml::NodePtr& s : v->find_children("string"))
                        types.push_back(s->inner_text());

                for (size_t i = 0; i < names.size(); i++) {
                    if (key == "outlets")
                        d.outlets.emplace_back(names[i],
                                               i < types.size() ? types[i] : "id");
                    else
                        d.actions.push_back(names[i]);
                }
            }
        }
        if (!d.name.empty()) out.push_back(std::move(d));
    }
    return out;
}

std::string Document::describe(bool include_all_objects) const {
    std::string out = path_ + "\n";
    out += "Interface Builder archive " + format_version();
    if (std::string t = system_target(); !t.empty()) out += ", deployment target " + t;
    out += "\n";

    std::vector<ObjectNode> objs = objects();
    std::vector<Connection> conns = connections();
    std::vector<ClassDescription> cls = classes();

    // Count the interesting classes rather than dumping 460 objects by default.
    std::map<std::string, int> counts;
    for (const ObjectNode& o : objs) counts[o.cls]++;

    out += "\n" + std::to_string(objs.size()) + " objects:\n";
    std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [c, n] : sorted) {
        out += "  " + std::to_string(n) + "x " + c + "\n";
        if (sorted.size() > 24 && &c == &sorted[23].first) {
            out += "  ...\n";
            break;
        }
    }

    // Windows and top-level views are what someone actually wants to see.
    out += "\nTop-level and windows:\n";
    for (const ObjectNode& o : objs) {
        bool interesting = o.depth <= 1 || o.cls == "NSWindowTemplate" ||
                           o.cls == "NSWindow" || o.cls == "NSCustomObject" ||
                           o.cls == "NSMenu";
        if (!interesting) continue;
        out += "  " + std::string(static_cast<size_t>(o.depth) * 2, ' ') +
               o.cls + " [" + o.id + "]";
        if (!o.title.empty()) out += "  " + o.title;
        out += "\n";
    }

    if (!cls.empty()) {
        out += "\nClasses the nib knows about:\n";
        for (const ClassDescription& c : cls) {
            out += "  " + c.name;
            if (!c.superclass.empty()) out += " : " + c.superclass;
            if (!c.source_file.empty()) out += "   (" + c.source_file + ")";
            out += "\n";
            for (const auto& [n, t] : c.outlets)
                out += "      outlet  " + n + " : " + t + "\n";
            for (const std::string& a : c.actions)
                out += "      action  " + a + ":\n";
        }
    }

    if (!conns.empty()) {
        out += "\n" + std::to_string(conns.size()) + " connections:\n";
        for (const Connection& c : conns) {
            out += "  " + c.kind + "  " + c.label + "   " +
                   (c.source_class.empty() ? c.source : c.source_class) + " -> " +
                   (c.dest_class.empty() ? c.destination : c.dest_class) + "\n";
        }
    }

    if (include_all_objects) {
        out += "\nFull object tree:\n";
        for (const ObjectNode& o : objs) {
            out += "  " + std::string(static_cast<size_t>(o.depth) * 2, ' ') +
                   o.cls + " [" + o.id + "]";
            if (!o.title.empty()) out += "  " + o.title;
            out += "\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------

bool Document::remove_class(const std::string& name, std::string* error) {
    xml::NodePtr classes = keyed(data(), "IBDocument.Classes");
    if (!classes) { if (error) *error = "no IBDocument.Classes section"; return false; }

    for (const xml::NodePtr& container : classes->children) {
        if (!container->is_element()) continue;
        for (const xml::NodePtr& pcd : container->find_children("object")) {
            if (pcd->attr("class") != "IBPartialClassDescription") continue;
            xml::NodePtr n = keyed(pcd, "className");
            if (n && n->inner_text() == name) {
                container->remove_child(pcd);
                return true;
            }
        }
    }
    if (error) *error = "no class named " + name + " is declared in this nib";
    return false;
}

bool Document::add_class(const ClassDescription& desc, std::string* error) {
    if (desc.name.empty()) {
        if (error) *error = "the class needs a name";
        return false;
    }
    xml::NodePtr classes = keyed(data(), "IBDocument.Classes");
    if (!classes) {
        if (error) *error = "this nib has no IBDocument.Classes section";
        return false;
    }

    // Replace any existing declaration so this is idempotent.
    remove_class(desc.name, nullptr);

    // In a nib with no custom classes yet, Interface Builder writes the section
    // as a bare self-closing <object class="IBClassDescriber"/> -- there is no
    // array to append to, so create the one it expects.
    xml::NodePtr container;
    for (const xml::NodePtr& c : classes->children) {
        if (!c->is_element()) continue;
        if (c->name == "object" && c->attr("class").find("Array") != std::string::npos) {
            container = c;
            break;
        }
    }
    if (!container) {
        container = xml::Node::element("object");
        container->set_attr("class", "NSMutableArray");
        container->set_attr("key", "referencedPartialClassDescriptions");
        auto flag = xml::Node::element("bool");
        flag->set_attr("key", "EncodedWithXMLCoder");
        flag->set_inner_text("YES");
        container->append(flag);
        classes->append(container);
    }

    auto pcd = xml::Node::element("object");
    pcd->set_attr("class", "IBPartialClassDescription");

    auto cn = xml::Node::element("string");
    cn->set_attr("key", "className");
    cn->set_inner_text(desc.name);
    pcd->append(cn);

    auto sc = xml::Node::element("string");
    sc->set_attr("key", "superclassName");
    sc->set_inner_text(desc.superclass.empty() ? "NSObject" : desc.superclass);
    pcd->append(sc);

    if (!desc.outlets.empty()) {
        std::vector<std::pair<std::string, std::string>> kv;
        for (const auto& [n, t] : desc.outlets)
            kv.emplace_back(n, t.empty() ? "id" : t);
        pcd->append(make_dict("outlets", kv));
    }

    if (!desc.actions.empty()) {
        // The value is the action's sender type, which is always id here.
        std::vector<std::pair<std::string, std::string>> kv;
        for (const std::string& a : desc.actions) kv.emplace_back(a, "id");
        pcd->append(make_dict("actions", kv));
    }

    if (!desc.source_file.empty()) {
        auto src = xml::Node::element("object");
        src->set_attr("class", "IBClassDescriptionSource");
        src->set_attr("key", "sourceIdentifier");
        auto major = xml::Node::element("string");
        major->set_attr("key", "majorKey");
        major->set_inner_text("IBProjectSource");
        src->append(major);
        auto rel = xml::Node::element("string");
        rel->set_attr("key", "relativePath");
        rel->set_inner_text(desc.source_file);
        src->append(rel);
        pcd->append(src);
    }

    container->append(pcd);
    return true;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

namespace {

bool open_doc(const json& a, ToolContext& ctx, Document* d, std::string* err) {
    std::string p = jstr(a, "path");
    if (p.empty()) { *err = "'path' is required"; return false; }
    return d->load(resolve_path(p, ctx.cwd), err);
}

} // namespace

void add_tools(ToolRegistry& registry) {
    {
        Tool t;
        t.spec.name = "xib_info";
        t.spec.description =
            "Inspect an Interface Builder .xib file: the object graph, windows "
            "and menus, the classes the nib expects with their outlets and "
            "actions, and every connection.\n"
            "\n"
            "Read this before writing controller code for a nib. The nib is the "
            "contract -- an outlet the nib declares but your class does not "
            "have, or an action wired to a method you never wrote, fails at "
            "runtime with an unhelpful message rather than at compile time.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "path":    {"type": "string",  "description": "Path to the .xib file."},
                "objects": {"type": "boolean", "description": "Include the full object tree, which can be long. Default false."}
            },
            "required": ["path"]
        })");
        t.kind = ToolKind::Read;
        t.source = "builtin";
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            Document d;
            std::string err;
            if (!open_doc(a, ctx, &d, &err)) return ToolResult::err(err);
            return ToolResult::ok(d.describe(jbool(a, "objects", false)));
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "xib_declare_class";
        t.spec.description =
            "Tell a nib about one of your classes, so Interface Builder offers "
            "its outlets and actions when wiring things up. Declaring the class "
            "is what has to happen before a controller can be connected to "
            "anything in the nib.\n"
            "\n"
            "This does not create the connections themselves -- doing that "
            "correctly requires synthesising several interlocking records, and a "
            "nib that is subtly wrong still opens and then misbehaves. Make the "
            "connections in Interface Builder, or instantiate the controller in "
            "code and set its outlets there.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "path":       {"type": "string", "description": "Path to the .xib file."},
                "class_name": {"type": "string", "description": "Your class, e.g. AppController."},
                "superclass": {"type": "string", "description": "Superclass. Default NSObject."},
                "source_file":{"type": "string", "description": "Header the class lives in, e.g. AppController.h"},
                "outlets": {
                    "type": "array",
                    "description": "Outlets to declare.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "name": {"type": "string"},
                            "type": {"type": "string", "description": "Class of the outlet, e.g. NSTextField. Default id."}
                        },
                        "required": ["name"]
                    }
                },
                "actions": {
                    "type": "array",
                    "description": "Action method names, without the trailing colon.",
                    "items": {"type": "string"}
                }
            },
            "required": ["path", "class_name"]
        })");
        t.kind = ToolKind::Mutate;
        t.source = "builtin";
        t.preview = [](const json& a) {
            return ToolPreview{"xib_declare_class  " + jstr(a, "class_name"),
                               "in " + jstr(a, "path")};
        };
        t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
            Document d;
            std::string err;
            if (!open_doc(a, ctx, &d, &err)) return ToolResult::err(err);

            ClassDescription desc;
            desc.name = jstr(a, "class_name");
            desc.superclass = jstr(a, "superclass", "NSObject");
            desc.source_file = jstr(a, "source_file");
            if (const json* o = jptr(a, "outlets"); o && o->is_array()) {
                for (const json& e : *o) {
                    std::string n = jstr(e, "name");
                    if (!n.empty()) desc.outlets.emplace_back(n, jstr(e, "type", "id"));
                }
            }
            if (const json* ac = jptr(a, "actions"); ac && ac->is_array()) {
                for (const json& e : *ac)
                    if (e.is_string()) desc.actions.push_back(e.get<std::string>());
            }

            if (!d.add_class(desc, &err)) return ToolResult::err(err);
            if (!d.save(&err)) return ToolResult::err(err);

            return ToolResult::ok(
                "Declared " + desc.name + " in " + d.path() + " with " +
                std::to_string(desc.outlets.size()) + " outlet(s) and " +
                std::to_string(desc.actions.size()) +
                " action(s). A backup was written alongside it.");
        };
        registry.add(std::move(t));
    }
}

} // namespace ppcode::xib

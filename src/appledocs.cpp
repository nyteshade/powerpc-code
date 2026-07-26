#include "appledocs.hpp"

#include "webtools.hpp"   // html_to_text, for the HTML inside declarations

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::appledocs {

namespace {

// A field separator that cannot occur in the data, so rows survive parsing.
constexpr const char* kSep = "\x1F";
constexpr const char* kRowSep = "\x1E";

std::string sqlite_binary() {
    for (const char* p : {"/opt/local/bin/sqlite3", "/usr/bin/sqlite3"})
        if (fs::exists(p)) return p;
    return "";
}

// Apple's short token-type codes.
std::string human_type(const std::string& code) {
    if (code == "cl")      return "class";
    if (code == "cat")     return "category";
    if (code == "intf")    return "protocol";
    if (code == "instm")   return "instance method";
    if (code == "clm")     return "class method";
    if (code == "intfm")   return "protocol method";
    if (code == "intfcm")  return "protocol class method";
    if (code == "instp")   return "property";
    if (code == "intfp")   return "protocol property";
    if (code == "clconst") return "class constant";
    if (code == "econst")  return "enum constant";
    if (code == "tdef")    return "typedef";
    if (code == "func")    return "function";
    if (code == "macro")   return "macro";
    if (code == "data")    return "global";
    if (code == "tag")     return "struct or enum";
    if (code == "binding") return "binding";
    return code;
}

// Map a friendly filter onto the codes it covers.
std::vector<std::string> type_codes(const std::string& filter) {
    std::string f = to_lower(trim(filter));
    if (f.empty()) return {};
    if (f == "class")     return {"cl"};
    if (f == "protocol")  return {"intf"};
    if (f == "category")  return {"cat"};
    if (f == "method")    return {"instm", "clm", "intfm", "intfcm"};
    if (f == "property")  return {"instp", "intfp"};
    if (f == "function")  return {"func"};
    if (f == "constant")  return {"econst", "clconst", "data"};
    if (f == "typedef")   return {"tdef"};
    if (f == "macro")     return {"macro"};
    return {f};   // let an exact code through
}

// SQL string literal escaping. The query reaches us from a model, so this is
// not optional.
std::string sql_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else if (c == '\0') continue;
        else out += c;
    }
    out += "'";
    return out;
}

// Shell single-quote escaping for the command we hand to sh -c.
std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Reject anything that is not plausibly an API name before it reaches SQL.
bool sane_name(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        if (c == '_' || c == ':' || c == '.' || c == '-' || c == ' ' ||
            c == '(' || c == ')' || c == '*' || c == '%')
            continue;
        return false;
    }
    return true;
}

std::string clean(const std::string& html) {
    if (html.empty()) return "";
    std::string text = web::html_to_text(html);
    // Declarations come wrapped in <PRE> and often carry hard line breaks.
    std::string flat;
    bool ws = false;
    for (char c : text) {
        if (c == '\n' || c == '\t' || c == ' ') {
            if (!ws) { flat += ' '; ws = true; }
        } else {
            flat += c;
            ws = false;
        }
    }
    return trim(flat);
}

std::vector<Entry> run_query(const std::string& index, const std::string& where,
                             int limit, std::string* error) {
    std::vector<Entry> out;
    std::string sqlite = sqlite_binary();
    if (sqlite.empty()) {
        if (error) *error = "sqlite3 is not installed; cannot read the docset index";
        return out;
    }

    std::string sql =
        "SELECT t.ZTOKENNAME, ty.ZTYPENAME, IFNULL(c.ZCONTAINERNAME,''), "
        "IFNULL(h.ZFRAMEWORKNAME,''), IFNULL(m.ZDECLARATION,''), "
        "IFNULL(m.ZABSTRACT,''), IFNULL(m.ZDEPRECATIONSUMMARY,'') "
        "FROM ZTOKEN t "
        "LEFT JOIN ZTOKENTYPE ty ON ty.Z_PK = t.ZTOKENTYPE "
        "LEFT JOIN ZCONTAINER c ON c.Z_PK = t.ZCONTAINER "
        "LEFT JOIN ZTOKENMETAINFORMATION m ON m.ZTOKEN = t.Z_PK "
        "LEFT JOIN ZHEADER h ON h.Z_PK = m.ZDECLAREDIN "
        "WHERE " + where + " LIMIT " + std::to_string(limit) + ";";

    std::string cmd = sh_quote(sqlite) + " -newline " + sh_quote(kRowSep) +
                      " -separator " + sh_quote(kSep) + " " + sh_quote(index) +
                      " " + sh_quote(sql) + " 2>/dev/null";

    CommandResult r = run_shell(cmd, ".", 20000, 2 * 1024 * 1024, nullptr);
    if (r.spawn_failed) {
        if (error) *error = r.error;
        return out;
    }

    std::string docset_name = fs::path(index).string();
    // .../com.apple.ADC_Reference_Library.CoreReference.docset/Contents/...
    size_t ds = docset_name.find(".docset");
    if (ds != std::string::npos) {
        size_t start = docset_name.rfind('/', ds);
        docset_name = docset_name.substr(start + 1, ds - start - 1);
        size_t dot = docset_name.rfind('.');
        if (dot != std::string::npos) docset_name = docset_name.substr(dot + 1);
    }

    for (const std::string& row : split(r.output, kRowSep[0])) {
        if (trim(row).empty()) continue;
        std::vector<std::string> f = split(row, kSep[0]);
        // split() drops a trailing empty field, and the last column
        // (deprecation) is empty for almost every API -- so requiring exactly
        // seven fields silently discarded every entry that was not deprecated.
        // Pad instead.
        if (f.size() < 2) continue;
        f.resize(7);
        Entry e;
        e.name = trim(f[0]);
        e.type = human_type(trim(f[1]));
        e.container = trim(f[2]);
        e.framework = trim(f[3]);
        e.declaration = clean(f[4]);
        e.abstract = clean(f[5]);
        e.deprecation = clean(f[6]);
        e.docset = docset_name;
        if (!e.name.empty()) out.push_back(std::move(e));
    }
    return out;
}

} // namespace

std::vector<std::string> docset_indexes() {
    std::vector<std::string> out;
    const char* roots[] = {
        "/Developer/Documentation/DocSets",
        "/Library/Developer/Shared/Documentation/DocSets",
    };
    for (const char* root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (!ends_with(e.path().string(), ".docset")) continue;
            std::string idx = e.path().string() + "/Contents/Resources/docSet.dsidx";
            if (fs::exists(idx, ec)) out.push_back(idx);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool available() { return !docset_indexes().empty() && !sqlite_binary().empty(); }

std::vector<Entry> search(const Query& q, std::string* error) {
    std::vector<Entry> out;
    std::string name = trim(q.name);
    if (!sane_name(name)) {
        if (error) *error = "that does not look like an API name";
        return out;
    }

    std::vector<std::string> indexes = docset_indexes();
    if (indexes.empty()) {
        if (error)
            *error = "no Apple documentation is installed under /Developer/Documentation";
        return out;
    }

    std::string type_clause;
    std::vector<std::string> codes = type_codes(q.type_filter);
    if (!codes.empty()) {
        type_clause = " AND ty.ZTYPENAME IN (";
        for (size_t i = 0; i < codes.size(); i++) {
            if (i) type_clause += ",";
            type_clause += sql_quote(codes[i]);
        }
        type_clause += ")";
    }
    std::string fw_clause;
    if (!trim(q.framework_filter).empty() && sane_name(q.framework_filter))
        fw_clause = " AND h.ZFRAMEWORKNAME = " + sql_quote(trim(q.framework_filter));

    // Three passes, widening only if the narrower one found nothing: an exact
    // hit should never be buried under substring noise.
    const std::string q_exact  = "t.ZTOKENNAME = " + sql_quote(name);
    const std::string q_prefix = "t.ZTOKENNAME LIKE " + sql_quote(name + "%");
    const std::string q_sub    = "t.ZTOKENNAME LIKE " + sql_quote("%" + name + "%");

    for (const std::string& where : {q_exact, q_prefix, q_sub}) {
        for (const std::string& idx : indexes) {
            std::string err;
            std::vector<Entry> got =
                run_query(idx, where + type_clause + fw_clause, q.limit, &err);
            if (!err.empty() && error) *error = err;
            for (Entry& e : got) {
                if (static_cast<int>(out.size()) >= q.limit) break;
                out.push_back(std::move(e));
            }
        }
        if (!out.empty()) break;
    }
    return out;
}

std::vector<std::string> search_headers(const std::string& name, int limit) {
    std::vector<std::string> out;
    if (!sane_name(name)) return out;

    // Only the system frameworks; scanning all of /usr/include as well would be
    // slow and mostly noise for Cocoa questions.
    std::string cmd =
        "grep -rn --include='*.h' -F " + sh_quote(name) +
        " /System/Library/Frameworks/*.framework/Headers 2>/dev/null | head -" +
        std::to_string(limit * 3);

    CommandResult r = run_shell(cmd, ".", 30000, 512 * 1024, nullptr);
    if (r.spawn_failed) return out;

    for (const std::string& line : split(r.output, '\n')) {
        std::string t = trim(line);
        if (t.empty()) continue;
        // Prefer declaration-looking lines over incidental mentions.
        out.push_back(elide(t, 220));
        if (static_cast<int>(out.size()) >= limit) break;
    }
    return out;
}

void add_tools(ToolRegistry& registry) {
    Tool t;
    t.spec.name = "apple_docs";
    t.spec.description =
        "Look up an Apple API in the developer documentation installed on this "
        "machine. Returns the exact declaration, what it belongs to, which "
        "framework it is in, a description, and any deprecation note.\n"
        "\n"
        "Use this whenever you are about to use a Cocoa, Carbon or Core "
        "Foundation API and are not certain it exists on this OS version. Your "
        "training data is dominated by far newer systems, so an API you remember "
        "may simply not be here -- and finding that out from a compile error "
        "costs minutes on this hardware. These docs shipped with this OS, so "
        "they are authoritative about what is actually available.\n"
        "\n"
        "Also searches the installed framework headers when the documentation "
        "has no entry.";
    t.spec.parameters = json::parse(R"({
        "type": "object",
        "properties": {
            "query":     {"type": "string", "description": "API name: a class (NSString), a method (stringWithFormat:), a function (CFRelease), a constant, or a prefix."},
            "type":      {"type": "string", "description": "Narrow by kind: class, protocol, category, method, property, function, constant, typedef, macro."},
            "framework": {"type": "string", "description": "Narrow by framework, e.g. Foundation, AppKit, CoreFoundation."},
            "limit":     {"type": "integer", "description": "Maximum entries to return. Default 12."}
        },
        "required": ["query"]
    })");
    t.kind = ToolKind::Read;
    t.source = "builtin";
    t.handler = [](const json& a, ToolContext& ctx) -> ToolResult {
        Query q;
        q.name = jstr(a, "query");
        if (q.name.empty()) return ToolResult::err("'query' is required");
        q.type_filter = jstr(a, "type");
        q.framework_filter = jstr(a, "framework");
        q.limit = static_cast<int>(jint(a, "limit", 12));
        if (q.limit <= 0 || q.limit > 50) q.limit = 12;

        if (ctx.note) ctx.note("apple_docs: " + q.name);

        std::string err;
        std::vector<Entry> hits = search(q, &err);

        std::string out;
        if (!hits.empty()) {
            for (const Entry& e : hits) {
                out += e.name;
                if (!e.container.empty()) out += "  (" + e.container + ")";
                out += "\n";
                out += "  kind:      " + e.type;
                if (!e.framework.empty()) out += ", " + e.framework;
                out += "\n";
                if (!e.declaration.empty())
                    out += "  declared:  " + e.declaration + "\n";
                if (!e.abstract.empty())
                    out += "  summary:   " + elide(e.abstract, 400) + "\n";
                if (!e.deprecation.empty())
                    out += "  DEPRECATED: " + elide(e.deprecation, 300) + "\n";
                out += "\n";
            }
        }

        // Headers as a complement: they are the ground truth for what the
        // compiler will actually accept.
        std::vector<std::string> hdr = search_headers(q.name, hits.empty() ? 12 : 4);
        if (!hdr.empty()) {
            out += hits.empty() ? "Not in the documentation index. Found in the "
                                  "installed framework headers:\n"
                                : "Also in the headers:\n";
            for (const std::string& h : hdr) out += "  " + h + "\n";
        }

        if (out.empty()) {
            std::string msg = "No documentation or header match for '" + q.name + "'";
            if (!err.empty()) msg += " (" + err + ")";
            msg += ". If this is an API you remember from a newer macOS, it may "
                   "simply not exist on this system.";
            return ToolResult::ok(msg);
        }
        return ToolResult::ok(out);
    };
    registry.add(std::move(t));
}

} // namespace ppcode::appledocs

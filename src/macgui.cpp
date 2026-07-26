#include "macgui.hpp"

#include "attach.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::macgui {

namespace {

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// An application name we are willing to hand to osascript.
bool sane_app_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        if (c == ' ' || c == '.' || c == '-' || c == '_') continue;
        return false;
    }
    return true;
}

std::string temp_png() {
    std::time_t t = std::time(nullptr);
    static int counter = 0;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "/tmp/ppcode-shot-%lld-%d.png",
                  static_cast<long long>(t), ++counter);
    return buf;
}

} // namespace

bool screencapture_available() {
    return fs::exists("/usr/sbin/screencapture") || fs::exists("/usr/bin/screencapture");
}

bool png_size(const std::string& path, int* w, int* h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char hdr[33] = {0};
    size_t n = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (n < 33) return false;
    if (std::memcmp(hdr, "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    if (std::memcmp(hdr + 12, "IHDR", 4) != 0) return false;
    // PNG stores these big-endian; assemble byte by byte so this is correct on
    // either endianness rather than relying on this machine being big-endian.
    auto be32 = [](const unsigned char* p) {
        return (static_cast<int>(p[0]) << 24) | (static_cast<int>(p[1]) << 16) |
               (static_cast<int>(p[2]) << 8) | static_cast<int>(p[3]);
    };
    *w = be32(hdr + 16);
    *h = be32(hdr + 20);
    return *w > 0 && *h > 0;
}

std::vector<std::string> running_apps() {
    std::vector<std::string> out;
    // System Events knows about GUI apps; ps would list daemons too.
    CommandResult r = run_shell(
        "osascript -e 'tell application \"System Events\" to get name of every "
        "process whose background only is false' 2>/dev/null",
        ".", 15000, 64 * 1024, nullptr);
    if (r.spawn_failed || r.exit_code != 0) return out;
    for (const std::string& part : split(trim(r.output), ',')) {
        std::string p = trim(part);
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

Capture capture(const CaptureOptions& opts) {
    Capture cap;
    if (!screencapture_available()) {
        cap.error = "screencapture is not present on this system";
        return cap;
    }

    std::string path = opts.out_path.empty() ? temp_png() : opts.out_path;
    std::error_code ec;
    if (fs::path(path).has_parent_path())
        fs::create_directories(fs::path(path).parent_path(), ec);

    // -x suppresses the shutter sound, which is noise on an unattended run.
    std::string flags = "-x";
    if (opts.include_cursor) flags += " -C";
    if (opts.delay_s > 0) flags += " -T " + std::to_string(opts.delay_s);

    std::string mode = to_lower(trim(opts.mode));
    std::string pre;

    if (mode == "window") {
        if (!opts.app.empty()) {
            if (!sane_app_name(opts.app)) {
                cap.error = "that does not look like an application name";
                return cap;
            }
            // Bring it forward first, or we capture whatever is on top.
            pre = "osascript -e 'tell application " + sh_quote(opts.app) +
                  " to activate' >/dev/null 2>&1; sleep 1; ";
        }
        // -o omits the window shadow, which otherwise adds transparent padding.
        flags += " -w -o";
    } else if (mode == "region") {
        if (opts.w <= 0 || opts.h <= 0) {
            cap.error = "region capture needs a positive width and height";
            return cap;
        }
        char r[128];
        std::snprintf(r, sizeof(r), " -R%d,%d,%d,%d", opts.x, opts.y, opts.w, opts.h);
        flags += r;
    } else if (mode != "screen") {
        cap.error = "mode must be screen, window, or region";
        return cap;
    }

    std::string cmd = pre + "screencapture " + flags + " " + sh_quote(path) + " 2>&1";
    CommandResult r = run_shell(cmd, ".", 60000, 64 * 1024, nullptr);
    if (r.spawn_failed) {
        cap.error = r.error;
        return cap;
    }
    if (!fs::exists(path, ec)) {
        std::string why = trim(r.output);
        cap.error = "screencapture produced no file" +
                    (why.empty() ? std::string() : ": " + why);
        if (mode == "window")
            cap.error +=
                ". Window capture needs a window to actually be on screen; if "
                "this is a headless or remote session there may be no desktop.";
        return cap;
    }

    cap.bytes = static_cast<size_t>(fs::file_size(path, ec));
    if (cap.bytes == 0) {
        fs::remove(path, ec);
        cap.error = "screencapture produced an empty file";
        return cap;
    }
    png_size(path, &cap.width, &cap.height);

    // A blank capture is common on this machine -- the display sleeps and
    // screencapture happily returns a solid black frame. Sending that to a
    // vision model costs real money to be told there is nothing there, so
    // detect it here instead.
    //
    // A uniform image compresses to almost nothing per pixel; a real desktop
    // does not. Measured: solid black 1280x1024 is ~0.014 bytes/pixel, whereas
    // an ordinary screenshot is an order of magnitude denser.
    if (cap.width > 0 && cap.height > 0) {
        double per_pixel = static_cast<double>(cap.bytes) /
                           (static_cast<double>(cap.width) * cap.height);
        cap.likely_blank = per_pixel < 0.03;
    }

    cap.path = path;
    cap.ok = true;
    return cap;
}

void add_tools(ToolRegistry& registry, bool vision_supported) {
    {
        Tool t;
        t.spec.name = "screenshot";
        t.spec.description =
            vision_supported
                ? "Take a screenshot of this Mac's screen and look at it. Use "
                  "this to check that an application you built actually launched, "
                  "that a window looks right, that controls are aligned, or to "
                  "read an error dialog. This is the only way to see what your "
                  "Cocoa code actually produced -- prefer it to guessing.\n"
                  "\n"
                  "Modes: 'screen' for the whole display, 'window' with an app "
                  "name to bring that application forward and capture its front "
                  "window, or 'region' with x/y/width/height."
                : "Take a screenshot of this Mac's screen and save it to a file. "
                  "NOTE: the current model cannot see images, so this returns the "
                  "path and dimensions only. Switch to a vision-capable model to "
                  "actually look at the result.";
        t.spec.parameters = json::parse(R"({
            "type": "object",
            "properties": {
                "mode":    {"type": "string", "enum": ["screen", "window", "region"],
                            "description": "What to capture. Default 'screen'."},
                "app":     {"type": "string", "description": "For mode 'window': the application to bring forward, e.g. Sample."},
                "x":       {"type": "integer", "description": "Region left edge."},
                "y":       {"type": "integer", "description": "Region top edge."},
                "width":   {"type": "integer", "description": "Region width."},
                "height":  {"type": "integer", "description": "Region height."},
                "delay":   {"type": "integer", "description": "Seconds to wait before capturing, to let a window finish drawing."},
                "cursor":  {"type": "boolean", "description": "Include the mouse pointer."},
                "path":    {"type": "string", "description": "Where to save it. Defaults to a temporary file."}
            }
        })");
        // Capturing the screen is a real-world side effect and can reveal
        // whatever is on the display, so it goes through the approval gate.
        t.kind = ToolKind::Execute;
        t.source = "builtin";
        t.preview = [](const json& a) {
            std::string m = jstr(a, "mode", "screen");
            std::string d = m;
            if (m == "window" && !jstr(a, "app").empty()) d += " of " + jstr(a, "app");
            return ToolPreview{"screenshot", d};
        };
        t.handler = [vision_supported](const json& a, ToolContext& ctx) -> ToolResult {
            CaptureOptions o;
            o.mode = jstr(a, "mode", "screen");
            o.app = jstr(a, "app");
            o.x = static_cast<int>(jint(a, "x"));
            o.y = static_cast<int>(jint(a, "y"));
            o.w = static_cast<int>(jint(a, "width"));
            o.h = static_cast<int>(jint(a, "height"));
            o.delay_s = static_cast<int>(jint(a, "delay", 0));
            o.include_cursor = jbool(a, "cursor", false);
            o.out_path = jstr(a, "path");
            if (!o.out_path.empty()) o.out_path = resolve_path(o.out_path, ctx.cwd);

            if (ctx.note) ctx.note("capturing " + o.mode);

            Capture c = capture(o);
            if (!c.ok) return ToolResult::err(c.error);

            char dims[128];
            std::snprintf(dims, sizeof(dims), "%dx%d, %zu bytes", c.width, c.height,
                          c.bytes);

            if (c.likely_blank) {
                // Do not spend a vision call on an empty frame.
                return ToolResult::ok(
                    "Captured " + c.path + " (" + dims + "), but it appears to be "
                    "a blank frame -- almost certainly the display is asleep or "
                    "showing a screensaver. The image has NOT been attached, "
                    "because there would be nothing in it to see.\n"
                    "\n"
                    "Wake the display first (move the mouse, or have the user "
                    "press a key on the machine), then capture again. If this is "
                    "a headless machine with no monitor attached, screenshots "
                    "will always be blank and you should not rely on them.");
            }

            if (!vision_supported) {
                return ToolResult::ok(
                    "Saved a screenshot to " + c.path + " (" + dims +
                    "). The current model cannot see images, so it has not been "
                    "shown to you. Switch to a vision-capable model to look at it.");
            }
            // The image itself is delivered as a follow-up user message by the
            // agent loop; this result tells it where to find it.
            return ToolResult::ok("PPCODE_ATTACH_IMAGE:" + c.path + "\n" +
                                  "Screenshot captured (" + dims + ").");
        };
        registry.add(std::move(t));
    }
    {
        Tool t;
        t.spec.name = "list_apps";
        t.spec.description =
            "List the GUI applications currently running, so you can name one for "
            "a window screenshot or check whether something you launched is still "
            "up.";
        t.spec.parameters = json::parse(R"({"type": "object", "properties": {}})");
        t.kind = ToolKind::Read;
        t.source = "builtin";
        t.handler = [](const json&, ToolContext&) -> ToolResult {
            std::vector<std::string> apps = running_apps();
            if (apps.empty())
                return ToolResult::ok(
                    "No GUI applications reported. There may be no desktop session "
                    "on this machine, or System Events may not be responding.");
            return ToolResult::ok("Running applications:\n  " + join(apps, "\n  "));
        };
        registry.add(std::move(t));
    }
}

} // namespace ppcode::macgui

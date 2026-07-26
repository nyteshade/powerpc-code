// macgui.hpp -- seeing and driving the Aqua desktop.
//
// The point of this platform is its GUI, and until now the model has been
// writing Cocoa code entirely blind. screencapture is present on 10.5, and the
// multimodal path already exists, so a vision-capable model can actually look at
// the window it just built: check the layout, spot a control that is misaligned,
// confirm the thing even launched.
//
// Screenshots are returned as an attachment rather than a file path, because a
// path is useless to the model on its own.
#pragma once

#include "common.hpp"
#include "openrouter.hpp"
#include "tools.hpp"

namespace ppcode::macgui {

bool screencapture_available();

struct CaptureOptions {
    std::string mode = "screen";   // "screen" | "window" | "region"
    std::string app;               // window mode: application name to bring forward
    int x = 0, y = 0, w = 0, h = 0;  // region mode
    bool include_cursor = false;
    int delay_s = 0;
    std::string out_path;          // empty means a temporary file
};

struct Capture {
    bool ok = false;
    std::string error;
    std::string path;
    size_t bytes = 0;
    int width = 0, height = 0;
    // The display sleeps on this machine and screencapture then returns a solid
    // black frame. Flagged so we do not pay a vision model to discover that.
    bool likely_blank = false;
};

Capture capture(const CaptureOptions& opts);

// PNG dimensions, read from the IHDR chunk. Big-endian fields, which on this
// machine happen to need no swapping.
bool png_size(const std::string& path, int* w, int* h);

// List running GUI applications, so a window capture can name one.
std::vector<std::string> running_apps();

// `vision_supported` gates whether a screenshot is attached or merely described.
void add_tools(ToolRegistry& registry, bool vision_supported);

} // namespace ppcode::macgui

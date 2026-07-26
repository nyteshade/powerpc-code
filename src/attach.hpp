// attach.hpp -- turning files and URLs into message content parts.
#pragma once

#include "common.hpp"
#include "openrouter.hpp"

namespace ppcode::attach {

// MIME type from the file's magic bytes, falling back to its extension.
// Returns empty when the type is not recognised.
std::string detect_mime(const std::string& path);

bool is_image_mime(const std::string& mime);

// Largest image we will base64 into a request. Data URIs inflate by ~4/3 and
// this machine is not fast at either reading or encoding.
constexpr size_t kMaxImageBytes = 5 * 1024 * 1024;
constexpr size_t kMaxTextBytes = 256 * 1024;

struct Loaded {
    bool ok = false;
    std::string error;
    ContentPart part;
    std::string note;      // e.g. "image not supported by this model, described instead"
};

// Load one attachment.
//   spec_path  local path or http(s) URL
//   kind       "auto" | "image" | "text"
//   detail     image detail hint
//   supports_images  false means degrade rather than send an image
//   cwd        base for relative paths
Loaded load(const std::string& spec_path, const std::string& kind,
            const std::string& detail, bool supports_images,
            const std::string& cwd);

} // namespace ppcode::attach

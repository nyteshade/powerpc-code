#include "attach.hpp"

#include "tools.hpp"   // resolve_path

#include <filesystem>

namespace fs = std::filesystem;

namespace ppcode::attach {

namespace {

bool starts_with_bytes(const std::string& s, const char* magic, size_t n) {
    if (s.size() < n) return false;
    return std::memcmp(s.data(), magic, n) == 0;
}

std::string mime_from_extension(const std::string& path) {
    std::string p = to_lower(path);
    size_t dot = p.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = p.substr(dot + 1);

    if (ext == "png")                      return "image/png";
    if (ext == "jpg" || ext == "jpeg")     return "image/jpeg";
    if (ext == "gif")                      return "image/gif";
    if (ext == "webp")                     return "image/webp";
    if (ext == "bmp")                      return "image/bmp";
    if (ext == "tif" || ext == "tiff")     return "image/tiff";
    if (ext == "pdf")                      return "application/pdf";
    if (ext == "json")                     return "application/json";
    if (ext == "md" || ext == "markdown")  return "text/markdown";
    if (ext == "txt" || ext == "log")      return "text/plain";
    if (ext == "html" || ext == "htm")     return "text/html";
    if (ext == "xml" || ext == "plist" || ext == "xib") return "text/xml";
    if (ext == "csv")                      return "text/csv";
    if (ext == "yaml" || ext == "yml")     return "text/yaml";
    return "";
}

} // namespace

std::string detect_mime(const std::string& path) {
    // Sniff the header first: extensions lie, especially on a filesystem with
    // resource forks and files copied from elsewhere.
    std::string head;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[64];
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        std::fclose(f);
        head.assign(buf, n);
    }

    if (!head.empty()) {
        if (starts_with_bytes(head, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
        if (starts_with_bytes(head, "\xFF\xD8\xFF", 3))      return "image/jpeg";
        if (starts_with_bytes(head, "GIF87a", 6) ||
            starts_with_bytes(head, "GIF89a", 6))            return "image/gif";
        if (head.size() >= 12 && starts_with_bytes(head, "RIFF", 4) &&
            std::memcmp(head.data() + 8, "WEBP", 4) == 0)    return "image/webp";
        if (starts_with_bytes(head, "BM", 2))                return "image/bmp";
        if (starts_with_bytes(head, "%PDF", 4))              return "application/pdf";
        // Big-endian and little-endian TIFF, both plausible on this platform.
        if (starts_with_bytes(head, "MM\x00\x2a", 4) ||
            starts_with_bytes(head, "II\x2a\x00", 4))        return "image/tiff";
    }
    return mime_from_extension(path);
}

bool is_image_mime(const std::string& mime) {
    return starts_with(mime, "image/");
}

Loaded load(const std::string& spec_path, const std::string& kind,
            const std::string& detail, bool supports_images,
            const std::string& cwd) {
    Loaded out;
    std::string want = to_lower(trim(kind.empty() ? "auto" : kind));

    // A remote image can be passed through as a URL; the provider fetches it,
    // which also saves this machine the download and the base64.
    if (starts_with(spec_path, "http://") || starts_with(spec_path, "https://")) {
        if (want == "text") {
            out.error = "text attachments must be local files; use web_fetch for URLs";
            return out;
        }
        if (!supports_images) {
            out.ok = true;
            out.part = ContentPart::make_text("[attachment omitted: " + spec_path +
                                              " is an image but this model cannot "
                                              "see images]");
            out.note = "model has no image input; URL described instead of attached";
            return out;
        }
        out.ok = true;
        out.part = ContentPart::make_image(spec_path, detail.empty() ? "auto" : detail);
        return out;
    }

    std::string full = resolve_path(spec_path, cwd);
    std::error_code ec;
    if (!fs::exists(full, ec)) {
        out.error = "attachment not found: " + spec_path;
        return out;
    }
    if (fs::is_directory(full, ec)) {
        out.error = "attachment is a directory: " + spec_path;
        return out;
    }
    uintmax_t size = fs::file_size(full, ec);

    std::string mime = detect_mime(full);
    bool as_image = (want == "image") || (want == "auto" && is_image_mime(mime));

    if (as_image) {
        if (!supports_images) {
            out.ok = true;
            out.part = ContentPart::make_text(
                "[attachment omitted: " + spec_path + " is an image (" +
                (mime.empty() ? "unknown type" : mime) +
                ") but the selected model cannot see images. Choose a "
                "vision-capable model to include it.]");
            out.note = spec_path + ": model has no image input";
            return out;
        }
        if (size > kMaxImageBytes) {
            out.error = spec_path + " is " + std::to_string(size / 1024) +
                        " KB; the limit for inline images is " +
                        std::to_string(kMaxImageBytes / 1024) + " KB";
            return out;
        }
        if (mime.empty()) {
            out.error = spec_path + ": could not determine an image type";
            return out;
        }

        std::string bytes, rerr;
        if (!read_file_text(full, &bytes, &rerr)) {
            out.error = rerr;
            return out;
        }
        ContentPart p = ContentPart::make_image(
            "data:" + mime + ";base64," + base64_encode(bytes),
            detail.empty() ? "auto" : detail);
        p.mime = mime;
        out.part = std::move(p);
        out.ok = true;
        return out;
    }

    // Everything else is inlined as text, which every model can use.
    if (size > kMaxTextBytes) {
        out.error = spec_path + " is " + std::to_string(size / 1024) +
                    " KB; inline text attachments are capped at " +
                    std::to_string(kMaxTextBytes / 1024) +
                    " KB. Have the model read it with read_file instead.";
        return out;
    }
    std::string text, rerr;
    if (!read_file_text(full, &text, &rerr)) {
        out.error = rerr;
        return out;
    }
    // A NUL byte means this is not text, whatever the extension claimed.
    if (text.find('\0') != std::string::npos) {
        out.error = spec_path + " looks binary and is not a recognised image type";
        return out;
    }

    ContentPart p = ContentPart::make_text(
        "===== attached file: " + spec_path + " =====\n" + text);
    p.mime = mime;
    out.part = std::move(p);
    out.ok = true;
    return out;
}

} // namespace ppcode::attach

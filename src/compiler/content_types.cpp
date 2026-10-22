#include "compiler/content_types.h"

#include <cctype>
#include <string>

namespace fourfold {

namespace {

struct TypeEntry {
    const char* extension;
    const char* contentType;
};

const TypeEntry kTypes[] = {
    // Text
    {".html", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".mjs", "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".map", "application/json"},
    {".webmanifest", "application/manifest+json"},
    {".txt", "text/plain; charset=utf-8"},
    {".csv", "text/csv; charset=utf-8"},
    // Images
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".webp", "image/webp"},
    {".avif", "image/avif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    // Fonts
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    // Audio and video
    {".mp3", "audio/mpeg"},
    {".ogg", "audio/ogg"},
    {".wav", "audio/wav"},
    {".mp4", "video/mp4"},
    {".webm", "video/webm"},
    // Other
    {".pdf", "application/pdf"},
    {".wasm", "application/wasm"},
    {".zip", "application/zip"},
};

}  // namespace

const char* contentTypeForExtension(std::string_view extension) {
    std::string lower;
    for (char c : extension) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const TypeEntry& t : kTypes) {
        if (lower == t.extension) return t.contentType;
    }
    return nullptr;
}

}  // namespace fourfold

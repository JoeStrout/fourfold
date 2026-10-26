// The engine's own files, under /__fourfold/ (Arch §5, pipeline): the htmx
// library with Fourfold's settings, and the default Markdown stylesheet.
// They're built into the program (cmake's fourfold_embed), so an app can't
// replace or break them.

#include "pipeline/stages.h"

namespace fourfold {

extern const unsigned char kHtmxLibrary[];
extern const size_t kHtmxLibrarySize;
extern const unsigned char kHtmxConfig[];
extern const size_t kHtmxConfigSize;
extern const unsigned char kMarkdownCss[];
extern const size_t kMarkdownCssSize;

namespace {

std::string bytes(const unsigned char* data, size_t size) {
    return std::string(reinterpret_cast<const char*>(data), size);
}

HttpResponse asset(const char* contentType, std::string body) {
    HttpResponse r;
    r.headers.set("Content-Type", contentType);
    // A day: long enough to matter, short enough that an engine upgrade
    // reaches browsers soon. (The URLs don't carry a version.)
    r.headers.set("Cache-Control", "public, max-age=86400");
    r.body = std::move(body);
    return r;
}

}  // namespace

const char* const kBuiltinMarkdownCssUrl = "/__fourfold/markdown.css";

std::optional<HttpResponse> builtinAsset(const std::string& path) {
    if (path == "/__fourfold/htmx.js") {
        static const std::string htmx =
            bytes(kHtmxLibrary, kHtmxLibrarySize) + "\n" + bytes(kHtmxConfig, kHtmxConfigSize);
        return asset("text/javascript; charset=utf-8", htmx);
    }
    if (path == kBuiltinMarkdownCssUrl) {
        return asset("text/css; charset=utf-8", bytes(kMarkdownCss, kMarkdownCssSize));
    }
    return std::nullopt;
}

}  // namespace fourfold

// The engine's own files, under /__fourfold/ (Arch §5, pipeline): the htmx
// library with Fourfold's settings, the proof-of-work solver, and the
// default Markdown stylesheet.
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
extern const unsigned char kChallengeScript[];
extern const size_t kChallengeScriptSize;

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
const char* const kChallengeScriptUrl = "/__fourfold/challenge.js";

std::optional<HttpResponse> builtinAsset(const std::string& path) {
    if (path == "/__fourfold/htmx.js") {
        // With the proof-of-work solver, so a form swapped in by htmx can
        // solve its puzzle.
        static const std::string htmx = bytes(kHtmxLibrary, kHtmxLibrarySize) + "\n" +
                                         bytes(kHtmxConfig, kHtmxConfigSize) + "\n" +
                                         bytes(kChallengeScript, kChallengeScriptSize);
        return asset("text/javascript; charset=utf-8", htmx);
    }
    if (path == kChallengeScriptUrl) {
        return asset("text/javascript; charset=utf-8", bytes(kChallengeScript, kChallengeScriptSize));
    }
    if (path == kBuiltinMarkdownCssUrl) {
        return asset("text/css; charset=utf-8", bytes(kMarkdownCss, kMarkdownCssSize));
    }
    return std::nullopt;
}

}  // namespace fourfold

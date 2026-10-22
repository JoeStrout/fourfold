// The routes pass: file-based routing for code/pages/ (overview §4).
//
//   pages/index.html          →  /
//   pages/about.html          →  /about
//   pages/docs/index.html     →  /docs/          (canonical with the slash; /docs redirects)
//   pages/img/logo.png        →  /img/logo.png   (static files keep their names)
//   pages/404.html            →  the not-found page (not a route)

#include <cctype>

#include "compiler/content_types.h"
#include "passes.h"

namespace fourfold {

namespace {

// First URL segments the engine keeps for itself (Arch §5, pipeline).
const char* const kReservedSegments[] = {"__auth", "__fourfold", "__dev"};

std::string lower(std::string_view s) {
    std::string result;
    for (char c : s) result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// The URL for a file at `segments` (below pages/), with the last segment
// replaced by `lastName` ("" drops it, for index pages).
std::string urlFor(const std::vector<std::string_view>& segments, std::string_view lastName) {
    std::string url;
    for (size_t i = 1; i + 1 < segments.size(); i++) {
        url += "/";
        url += segments[i];
    }
    if (!lastName.empty()) {
        url += "/";
        url += lastName;
    }
    return url.empty() ? "/" : url;
}

void addRoute(AppImage& image, Diagnostics& diagnostics, const std::string& url, RouteEntry entry) {
    const RouteEntry* existing = image.routes.find(url);
    if (existing) {
        diagnostics.error(codeFileName(entry.source), "URL " + url + " is also served by " +
                                                          codeFileName(existing->source));
        return;
    }
    image.routes.add(url, std::move(entry));
}

}  // namespace

void routesPass(const CodeListing& listing, AppImage& image, Diagnostics& diagnostics) {
    const RelPath pages = RelPath::parse("pages").value();
    bool anyPages = false;

    for (const CodeEntry& file : listing.files) {
        if (!file.path.isWithin(pages)) continue;
        anyPages = true;
        std::vector<std::string_view> segments = file.path.segments();
        std::string fileName = codeFileName(file.path);

        if (segments.size() > 1) {
            bool reserved = false;
            for (const char* r : kReservedSegments) reserved = reserved || segments[1] == r;
            if (reserved) {
                diagnostics.error(fileName, "/" + std::string(segments[1]) +
                                                "/ is reserved for Fourfold's own pages");
                continue;
            }
        }

        std::string extension = lower(file.path.extension());
        std::string_view name = file.path.name();

        if (extension == ".html") {
            if (segments.size() == 2 && name == "404.html") {
                image.notFoundPage = file.path;
                continue;
            }
            std::string_view stem = name.substr(0, name.size() - extension.size());
            bool isIndex = stem == "index";
            std::string url = urlFor(segments, isIndex ? std::string_view() : stem);
            addRoute(image, diagnostics, url, {RouteKind::Page, file.path, isIndex && url != "/"});
        } else if (extension == ".ms" || extension == ".md") {
            diagnostics.warning(fileName, "MiniScript and Markdown pages aren't supported yet; ignored");
        } else if (const char* type = contentTypeForExtension(extension)) {
            addRoute(image, diagnostics, urlFor(segments, name), {RouteKind::Static, file.path});
            image.statics.add(file.path, {type, file.size, file.modifiedNs});
        } else {
            diagnostics.warning(fileName, extension.empty()
                                              ? "file has no extension, so no known type; not served"
                                              : "unknown file type " + extension + "; not served");
        }
    }

    if (!anyPages) diagnostics.warning("code/pages", "no pages found; the app serves nothing");
}

}  // namespace fourfold

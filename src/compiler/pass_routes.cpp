// The routes pass: file-based routing for code/pages/ (overview §4).
//
//   pages/index.html          →  /
//   pages/about.html          →  /about           (with render from about.ms, if it has one)
//   pages/docs/index.html     →  /docs/           (canonical with the slash; /docs redirects)
//   pages/img/logo.png        →  /img/logo.png    (static files keep their names)
//   pages/guestbook.ms        →  /guestbook/sign  for post.sign; /guestbook/more for get.more
//   pages/docs/index.ms       →  /docs/more       for get.more
//   pages/api.ms              →  /api             if it has a render but there's no api.html
//   pages/guide.md            →  /guide           rendered Markdown (see the markdown pass)
//   pages/404.html            →  the not-found page (not a route)
//   pages/500.html            →  the error page (not a route)

#include <algorithm>
#include <set>

#include "compiler/content_types.h"
#include "passes.h"

namespace fourfold {

namespace {

// First URL segments the engine keeps for itself (Arch §5, pipeline).
const char* const kReservedSegments[] = {"__auth", "__fourfold", "__dev"};

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

// `what` names the thing being routed in a collision message, e.g. "post.sign".
void addRoute(CompiledApp& app, Diagnostics& diagnostics, const std::string& url, RouteEntry entry,
              const std::string& what = "") {
    const RouteEntry* existing = app.routes.find(url);
    if (existing) {
        diagnostics.error(codeFileName(entry.source), (what.empty() ? "" : what + ": ") + "URL " + url +
                                                          " is also served by " + codeFileName(existing->source));
        return;
    }
    app.routes.add(url, std::move(entry));
}

std::string_view stemOf(const RelPath& path) {
    std::string_view name = path.name();
    return name.substr(0, name.size() - path.extension().size());
}

// "pages/about.html" → "pages/about.ms" (and the reverse).
RelPath withExtension(const RelPath& path, const char* extension) {
    std::string s = path.str();
    s.resize(s.size() - path.extension().size());
    return RelPath::parse(s + extension).value();
}

bool isSpecialPage(const std::vector<std::string_view>& segments, std::string_view stem) {
    return segments.size() == 2 && (stem == "404" || stem == "500");
}

void routeHandlers(const RelPath& source, const std::vector<std::string_view>& segments, std::string_view stem,
                   const PageHandlers& handlers, CompiledApp& app, Diagnostics& diagnostics) {
    // index.ms's handlers live in its folder; other files' below their name.
    std::string base = urlFor(segments, stem == "index" ? std::string_view() : stem);
    if (base == "/") base.clear();

    std::map<std::string, RouteEntry> entries;   // by handler name: get.x and post.x share a URL
    auto add = [&](HandlerRef::Kind kind, const std::string& name) {
        auto [it, isNew] = entries.try_emplace(name, RouteEntry{RouteKind::Handler, source});
        it->second.usesSession = handlers.usesSession;
        HandlerRef ref{kind, source, name};
        (kind == HandlerRef::Kind::Get ? it->second.get : it->second.post) = ref;
    };
    for (const std::string& name : handlers.get) add(HandlerRef::Kind::Get, name);
    for (const std::string& name : handlers.post) add(HandlerRef::Kind::Post, name);

    for (auto& [name, entry] : entries) {
        std::string what = entry.get ? entry.get->label() : entry.post->label();
        if (entry.get && entry.post) what += " and " + entry.post->label();
        addRoute(app, diagnostics, base + "/" + name, std::move(entry), what);
    }
}

}  // namespace

void routesPass(const CodeListing& listing, const std::map<RelPath, PageHandlers>& handlers, CompiledApp& app,
                Diagnostics& diagnostics) {
    const RelPath pages = RelPath::parse("pages").value();
    bool anyPages = false;

    std::set<RelPath> htmlFiles;
    for (const CodeEntry& file : listing.files) {
        if (file.path.isWithin(pages) && lowerExtension(file.path) == ".html") htmlFiles.insert(file.path);
    }

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

        std::string extension = lowerExtension(file.path);
        std::string_view name = file.path.name();
        std::string_view stem = stemOf(file.path);
        bool isIndex = stem == "index";

        if (extension == ".html") {
            if (isSpecialPage(segments, stem)) {
                (stem == "404" ? app.notFoundPage : app.errorPage) = file.path;
                continue;
            }
            std::string url = urlFor(segments, isIndex ? std::string_view() : stem);
            RouteEntry entry{RouteKind::Page, file.path, isIndex && url != "/"};
            auto code = handlers.find(withExtension(file.path, ".ms"));
            if (code != handlers.end() && code->second.render) {
                entry.render = HandlerRef{HandlerRef::Kind::Render, code->first, "render"};
                entry.usesSession = code->second.usesSession;
            }
            if (const CompiledTemplate* t = app.templates.find(file.path)) {
                std::vector<std::string> fields = t->fieldNames();
                if (std::find(fields.begin(), fields.end(), "flash") != fields.end()) entry.usesSession = true;
            }
            addRoute(app, diagnostics, url, std::move(entry));
        } else if (extension == ".ms") {
            auto found = handlers.find(file.path);
            if (found == handlers.end()) continue;   // it didn't compile; the scripts pass said why
            const PageHandlers& h = found->second;
            if (isSpecialPage(segments, stem)) {
                if (h.render || !h.get.empty() || !h.post.empty()) {
                    diagnostics.warning(fileName, "the " + std::string(stem) +
                                                      " page can't have code yet; its handlers are ignored");
                }
                continue;
            }
            bool hasHtml = htmlFiles.count(withExtension(file.path, ".html")) > 0;
            if (h.render && !hasHtml) {
                // A page made only of code: its render returns text, JSON, a redirect...
                std::string url = urlFor(segments, isIndex ? std::string_view() : stem);
                RouteEntry entry{RouteKind::Page, file.path, isIndex && url != "/"};
                entry.render = HandlerRef{HandlerRef::Kind::Render, file.path, "render"};
                entry.usesSession = h.usesSession;
                addRoute(app, diagnostics, url, std::move(entry), "render");
            }
            routeHandlers(file.path, segments, stem, h, app, diagnostics);
            if (!h.render && h.get.empty() && h.post.empty() && !hasHtml) {
                diagnostics.warning(fileName, "defines no render, get or post handlers, so nothing is served");
            }
        } else if (extension == ".md") {
            if (isSpecialPage(segments, stem)) {
                diagnostics.warning(fileName, "the " + std::string(stem) + " page must be " + std::string(stem) +
                                                  ".html; this one is ignored");
                continue;
            }
            std::string url = urlFor(segments, isIndex ? std::string_view() : stem);
            addRoute(app, diagnostics, url, {RouteKind::Markdown, file.path, isIndex && url != "/"});
        } else if (const char* type = contentTypeForExtension(extension)) {
            addRoute(app, diagnostics, urlFor(segments, name), {RouteKind::Static, file.path});
            app.statics.add(file.path, {type, file.size, file.modifiedNs});
        } else {
            diagnostics.warning(fileName, extension.empty()
                                              ? "file has no extension, so no known type; not served"
                                              : "unknown file type " + extension + "; not served");
        }
    }

    if (!anyPages) diagnostics.warning("code/pages", "no pages found; the app serves nothing");
}

}  // namespace fourfold

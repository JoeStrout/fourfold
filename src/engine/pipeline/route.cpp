#include "pipeline/stages.h"

namespace fourfold {

RouteDecision route(const AdmittedRequest& request, const AppImage& image) {
    RouteDecision decision;
    const RouteEntry* entry = image.routes.find(request.path.path);
    if (!entry) return decision;   // NotFound

    if (request.method != "GET" && request.method != "HEAD") {
        decision.kind = RouteDecision::Kind::MethodNotAllowed;
        return decision;
    }

    // Each route has one canonical URL, with or without a trailing slash, so
    // relative links in a page resolve against its source folder (/docs/ for
    // docs/index.html, /docs/intro for docs/intro.html). Redirect the other
    // form. The Location is rebuilt from the canonical path, never copied
    // from the request, so "//evil.com" can't become an off-site redirect.
    if (entry->trailingSlash != request.path.trailingSlash) {
        decision.kind = RouteDecision::Kind::Redirect;
        decision.location = encodeUrlPath(request.path.path);
        if (entry->trailingSlash) decision.location += "/";
        if (request.hasQuery) decision.location += "?" + request.query;
        return decision;
    }

    decision.kind = RouteDecision::Kind::Found;
    decision.entry = entry;
    return decision;
}

}  // namespace fourfold

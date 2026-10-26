#include "pipeline/stages.h"

#include <cctype>

namespace fourfold {

namespace {

bool isGet(const std::string& method) {
    return method == "GET" || method == "HEAD";
}

bool endsWithHtml(const std::string& path) {
    if (path.size() < 6) return false;
    std::string ext = path.substr(path.size() - 5);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".html";
}

// A request for a page by its file name, as a site converted to Fourfold
// (or a bookmark into it) may make: /about.html for /about, and
// /docs/index.html for /docs/. Redirects there, if there's such a page.
RouteDecision fileNameRedirect(const AdmittedRequest& request, const CompiledApp& app) {
    RouteDecision decision;   // NotFound
    const std::string& path = request.path.path;
    if (!isGet(request.method) || request.path.trailingSlash || !endsWithHtml(path)) return decision;
    std::string page = path.substr(0, path.size() - 5);
    bool index = page.size() >= 6 && page.compare(page.size() - 6, 6, "/index") == 0;
    if (index) page.resize(page.size() - 6);   // "/docs/index" → "/docs"
    if (page.empty()) page = "/";
    const RouteEntry* entry = app.routes.find(page);
    if (!entry || (entry->kind != RouteKind::Page && entry->kind != RouteKind::Markdown)) return decision;
    decision.kind = RouteDecision::Kind::Redirect;
    decision.location = encodeUrlPath(page);
    if (entry->trailingSlash) decision.location += "/";
    if (request.hasQuery) decision.location += "?" + request.query;
    return decision;
}

}  // namespace

RouteDecision route(const AdmittedRequest& request, const CompiledApp& app) {
    RouteDecision decision;
    const RouteEntry* entry = app.routes.find(request.path.path);
    if (!entry) return fileNameRedirect(request, app);   // or NotFound

    // Which methods this route answers, and what handles each.
    const HandlerRef* handler = nullptr;
    bool allowed = false;
    if (entry->kind == RouteKind::Handler) {
        if (isGet(request.method) && entry->get) handler = &*entry->get;
        if (request.method == "POST" && entry->post) handler = &*entry->post;
        allowed = handler != nullptr;
        decision.allow = entry->get ? "GET, HEAD" : "";
        if (entry->post) decision.allow += decision.allow.empty() ? "POST" : ", POST";
    } else {
        allowed = isGet(request.method);
        if (entry->render) handler = &*entry->render;
        decision.allow = "GET, HEAD";
    }
    if (!allowed) {
        decision.kind = RouteDecision::Kind::MethodNotAllowed;
        return decision;
    }

    // Each route has one canonical URL, with or without a trailing slash, so
    // relative links in a page resolve against its source folder (/docs/ for
    // docs/index.html, /docs/intro for docs/intro.html). Redirect the other
    // form, for GET; a POST to the wrong form isn't redirected (that would
    // turn it into a GET), just not found. The Location is rebuilt from the
    // canonical path, never copied from the request, so "//evil.com" can't
    // become an off-site redirect.
    if (entry->trailingSlash != request.path.trailingSlash) {
        if (!isGet(request.method)) return decision;   // NotFound
        decision.kind = RouteDecision::Kind::Redirect;
        decision.location = encodeUrlPath(request.path.path);
        if (entry->trailingSlash) decision.location += "/";
        if (request.hasQuery) decision.location += "?" + request.query;
        return decision;
    }

    decision.kind = RouteDecision::Kind::Found;
    decision.entry = entry;
    decision.handler = handler;
    return decision;
}

}  // namespace fourfold

#include "pipeline/stages.h"

namespace fourfold {

namespace {

bool isGet(const std::string& method) {
    return method == "GET" || method == "HEAD";
}

}  // namespace

RouteDecision route(const AdmittedRequest& request, const CompiledApp& app) {
    RouteDecision decision;
    const RouteEntry* entry = app.routes.find(request.path.path);
    if (!entry) return decision;   // NotFound

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

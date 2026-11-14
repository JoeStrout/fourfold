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

// A URL rule (code/routes.ms, M11.2), for a URL no route serves.
std::optional<RouteDecision> ruleDecision(const AdmittedRequest& request, const CompiledApp& app) {
    if (app.urlRules.empty()) return std::nullopt;
    UrlValues values;
    const UrlRule* rule = app.matchUrlRule(request.path, values);
    if (!rule) return std::nullopt;
    RouteDecision decision;
    decision.rule = rule;
    if (!isGet(request.method)) {
        decision.kind = RouteDecision::Kind::MethodNotAllowed;
        decision.allow = "GET, HEAD";
        return decision;
    }
    std::string filled = rule->target.fill(values);
    if (rule->kind == UrlRule::Kind::Redirect) {
        // The visitor's query is kept, after any the target adds (and before
        // its fragment).
        if (request.hasQuery && !request.query.empty()) {
            size_t hash = filled.find('#');
            std::string fragment = hash == std::string::npos ? "" : filled.substr(hash);
            filled.resize(filled.size() - fragment.size());
            filled += (filled.find('?') == std::string::npos ? "?" : "&") + request.query + fragment;
        }
        decision.kind = RouteDecision::Kind::Redirect;
        decision.location = std::move(filled);
        decision.status = rule->status;
        return decision;
    }

    // A rewrite: the target route, at the visitor's URL.
    std::string routeUrl = rule->routePath;
    const RouteEntry* target =
        rule->markdownAlias ? app.routes.findMarkdown(rule->routePath, &routeUrl) : app.routes.find(rule->routePath);
    if (!target) return std::nullopt;   // checked at compile time, so not expected
    auto entry = std::make_shared<const RouteEntry>(app.rewriteEntry(*target, routeUrl, request.path.path));
    decision.kind = RouteDecision::Kind::Found;
    decision.entry = entry.get();
    if (entry->kind == RouteKind::Handler) {
        decision.handler = entry->get ? &*entry->get : nullptr;
    } else if (entry->render) {
        decision.handler = &*entry->render;
    }
    decision.rewritten = std::move(entry);
    decision.markdown = rule->markdownAlias;
    if (decision.entry->kind == RouteKind::Markdown) {
        decision.markdownUrl = rule->markdownAlias ? rule->routePath
                                                   : markdownUrlFor(routeUrl, decision.entry->trailingSlash);
    }
    std::string query;
    size_t question = filled.find('?');
    if (question != std::string::npos) {
        query = filled.substr(question + 1);
        size_t hash = query.find('#');
        if (hash != std::string::npos) query.resize(hash);
    }
    if (request.hasQuery && !request.query.empty()) query += (query.empty() ? "" : "&") + request.query;
    decision.query = std::move(query);
    return decision;
}

}  // namespace

RouteDecision route(const AdmittedRequest& request, const CompiledApp& app) {
    RouteDecision decision;
    const RouteEntry* entry = app.routes.find(request.path.path);
    // A Markdown page's Markdown URL: the same route, as Markdown. Its one
    // canonical form has no trailing slash.
    bool markdown = false;
    if (!entry) {
        entry = app.routes.findMarkdown(request.path.path);
        markdown = entry != nullptr;
    }
    if (!entry) {
        if (auto ruled = ruleDecision(request, app)) return std::move(*ruled);
        return fileNameRedirect(request, app);   // or NotFound
    }

    // Which methods this route answers, and what handles each.
    const HandlerRef* handler = nullptr;
    bool allowed = false;
    if (entry->kind == RouteKind::Handler) {
        if (isGet(request.method) && entry->get) handler = &*entry->get;
        if (request.method == "POST" && entry->post) handler = &*entry->post;
        if (request.method == "POST" && entry->webhook) handler = &*entry->webhook;
        allowed = handler != nullptr;
        decision.allow = entry->get ? "GET, HEAD" : "";
        if (entry->post || entry->webhook) decision.allow += decision.allow.empty() ? "POST" : ", POST";
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
    bool trailingSlash = entry->trailingSlash && !markdown;
    if (trailingSlash != request.path.trailingSlash) {
        if (!isGet(request.method)) return decision;   // NotFound
        decision.kind = RouteDecision::Kind::Redirect;
        decision.location = encodeUrlPath(request.path.path);
        if (trailingSlash) decision.location += "/";
        if (request.hasQuery) decision.location += "?" + request.query;
        return decision;
    }

    decision.kind = RouteDecision::Kind::Found;
    decision.entry = entry;
    decision.handler = handler;
    decision.markdown = markdown;
    if (entry->kind == RouteKind::Markdown) {
        decision.markdownUrl = markdown ? request.path.path : markdownUrlFor(request.path.path, entry->trailingSlash);
    }
    return decision;
}

}  // namespace fourfold

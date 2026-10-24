#include "pipeline/stages.h"

namespace fourfold {

namespace {

const char* const kHtmlType = "text/html; charset=utf-8";

// A page template with no fields filled (missing fields render empty).
HttpResponse page(int status, const CompiledTemplate& compiled) {
    HttpResponse r;
    r.status = status;
    r.headers.set("Content-Type", kHtmlType);
    r.body = renderTemplate(compiled, {});
    return r;
}

HttpResponse notFound(const CompiledApp& app) {
    if (app.notFoundPage) {
        if (const CompiledTemplate* t = app.templates.find(*app.notFoundPage)) return page(404, *t);
    }
    return HttpResponse::text(404, "Not Found\n");
}

}  // namespace

HttpResponse dispatch(const RouteDecision& decision, const CompiledApp& app, const CodeTree& code) {
    switch (decision.kind) {
        case RouteDecision::Kind::NotFound:
            return notFound(app);

        case RouteDecision::Kind::MethodNotAllowed: {
            HttpResponse r = HttpResponse::text(405, "Method Not Allowed\n");
            r.headers.set("Allow", decision.allow);
            return r;
        }

        case RouteDecision::Kind::Redirect: {
            // 302, not 301: browsers cache a 301 indefinitely, which would
            // outlive a dev's change from docs.html to docs/index.html.
            HttpResponse r = HttpResponse::text(302, "Found\n");
            r.headers.set("Location", decision.location);
            return r;
        }

        case RouteDecision::Kind::Found:
            break;
    }

    const RouteEntry& entry = *decision.entry;
    if (entry.kind == RouteKind::Page) {
        // Pages are served from their compiled templates, not reread.
        const CompiledTemplate* t = app.templates.find(entry.source);
        return t ? page(200, *t) : notFound(app);
    }

    // The file was listed when the app was compiled; it may have gone since.
    // Until recompiling arrives (M3), treat that as not found.
    auto file = code.read(entry.source);
    if (!file) return notFound(app);
    HttpResponse r;
    const StaticFile* info = app.statics.find(entry.source);
    r.headers.set("Content-Type", info ? info->contentType : "application/octet-stream");
    r.body = file.value().contents();
    return r;
}

}  // namespace fourfold

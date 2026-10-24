#include "pipeline/stages.h"

namespace fourfold {

namespace {

const char* const kHtmlType = "text/html; charset=utf-8";

HttpResponse notFound(const CompiledApp& app, const CodeTree& code) {
    if (app.notFoundPage) {
        auto page = code.read(*app.notFoundPage);
        if (page) {
            HttpResponse r;
            r.status = 404;
            r.headers.set("Content-Type", kHtmlType);
            r.body = page.value().contents();
            return r;
        }
    }
    return HttpResponse::text(404, "Not Found\n");
}

}  // namespace

HttpResponse dispatch(const RouteDecision& decision, const CompiledApp& app, const CodeTree& code) {
    switch (decision.kind) {
        case RouteDecision::Kind::NotFound:
            return notFound(app, code);

        case RouteDecision::Kind::MethodNotAllowed: {
            HttpResponse r = HttpResponse::text(405, "Method Not Allowed\n");
            r.headers.set("Allow", "GET, HEAD");
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
    // The file was listed when the app was compiled; it may have gone
    // since. Until recompiling arrives (M3), treat that as not found.
    auto file = code.read(entry.source);
    if (!file) return notFound(app, code);

    HttpResponse r;
    if (entry.kind == RouteKind::Page) {
        r.headers.set("Content-Type", kHtmlType);
    } else {
        const StaticFile* info = app.statics.find(entry.source);
        r.headers.set("Content-Type", info ? info->contentType : "application/octet-stream");
    }
    r.body = file.value().contents();
    return r;
}

}  // namespace fourfold

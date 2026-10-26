#include "pipeline/stages.h"

namespace fourfold {

namespace {

const char* const kHtmlType = "text/html; charset=utf-8";

HttpResponse html(int status, std::string body) {
    HttpResponse r;
    r.status = status;
    r.headers.set("Content-Type", kHtmlType);
    r.body = std::move(body);
    return r;
}

// A page template with no code: its fields render empty, on purpose, so
// there's nothing to warn about (overview §4).
HttpResponse page(int status, const CompiledTemplate& compiled, const RenderOptions& render) {
    RenderOptions quiet = render;
    quiet.warnings = false;
    return html(status, fourfold::render(compiled, EmptyFields(), quiet).html.str());
}

HttpResponse notFound(const CompiledApp& app, const RenderOptions& render) {
    if (app.notFoundPage) {
        if (const CompiledTemplate* t = app.templates.find(*app.notFoundPage)) return page(404, *t, render);
    }
    return HttpResponse::text(404, "Not Found\n");
}

// A Markdown page, in the app's layout or the built-in one.
HttpResponse markdownPage(const MarkdownPage& md, const CompiledApp& app, const RenderOptions& render) {
    if (app.markdownLayout) {
        MapFields fields;
        fields.set("content", FieldValue::markup(md.content));
        fields.set("title", md.title.empty() ? FieldValue::null() : FieldValue::string(md.title));
        RenderOptions quiet = render;
        quiet.warnings = false;
        RenderResult r = fourfold::render(*app.markdownLayout, fields, quiet);
        if (r.ok()) return html(200, r.html.str());
        return HttpResponse::text(500, "Internal Server Error\n");
    }
    std::string body = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
                       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
                       escapeHtml(md.title) + "</title>\n<link rel=\"stylesheet\" href=\"" +
                       escapeHtml(render.markdownCss) +
                       "\">\n<style>body{max-width:46rem;margin:2rem auto;padding:0 1rem;"
                       "font-family:system-ui,sans-serif}</style>\n</head>\n<body>\n" +
                       md.content.str() + "</body></html>\n";
    return html(200, std::move(body));
}

}  // namespace

RenderOptions renderOptionsFor(const CompiledApp& app, std::string nonce) {
    RenderOptions o;
    o.nonce = std::move(nonce);
    o.markdownCss = app.hasMarkdownCss ? "/markdown.css" : kBuiltinMarkdownCssUrl;
    return o;
}

HttpResponse dispatch(const RouteDecision& decision, const CompiledApp& app, const CodeTree& code,
                      const RenderOptions& render) {
    switch (decision.kind) {
        case RouteDecision::Kind::NotFound:
            return notFound(app, render);

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
        return t ? page(200, *t, render) : notFound(app, render);
    }
    if (entry.kind == RouteKind::Markdown) {
        auto md = app.markdownPages.find(entry.source);
        return md != app.markdownPages.end() ? markdownPage(md->second, app, render) : notFound(app, render);
    }

    // The file was listed when the app was compiled; it may have gone since
    // (the next compile will drop its route).
    auto file = code.read(entry.source);
    if (!file) return notFound(app, render);
    HttpResponse r;
    const StaticFile* info = app.statics.find(entry.source);
    r.headers.set("Content-Type", info ? info->contentType : "application/octet-stream");
    r.body = file.value().contents();
    return r;
}

}  // namespace fourfold

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

// A page template with no code: its fields render empty (or as `fields`
// has them), on purpose, so there's nothing to warn about (overview §4).
HttpResponse page(int status, const CompiledTemplate& compiled, const RenderOptions& render,
                  const FieldSource* fields = nullptr) {
    RenderOptions quiet = render;
    quiet.warnings = false;
    EmptyFields none;
    return html(status, fourfold::render(compiled, fields ? *fields : none, quiet).html.str());
}

HttpResponse notFound(const CompiledApp& app, const RenderOptions& render) {
    if (app.notFoundPage) {
        if (const CompiledTemplate* t = app.templates.find(*app.notFoundPage)) return page(404, *t, render);
    }
    return HttpResponse::text(404, "Not Found\n");
}

// A Markdown page with no code: as HTML, in the app's layout or the
// built-in one; or as Markdown. Its fields render empty (or as `fields` has
// them), on purpose.
HttpResponse markdownPage(const CompiledTemplate& page, const CompiledApp& app, const RenderOptions& render,
                          const FieldSource* fields) {
    RenderOptions quiet = render;
    quiet.warnings = false;
    EmptyFields none;
    const CompiledTemplate* layout = app.markdownLayout ? &*app.markdownLayout : nullptr;
    RenderResult r = renderMarkdownPage(page, layout, fields ? *fields : none, quiet);
    if (!r.ok()) return HttpResponse::text(500, "Internal Server Error\n");
    if (render.format == OutputFormat::Markdown) {
        HttpResponse response;
        response.headers.set("Content-Type", kMarkdownContentType);
        response.body = std::move(r.markdown);
        return response;
    }
    return html(200, r.html.str());
}

}  // namespace

void setRenderFormat(RenderOptions& render, const RouteDecision& decision) {
    render.format = decision.markdown ? OutputFormat::Markdown : OutputFormat::Html;
    render.markdownAlternate =
        decision.markdown || decision.markdownUrl.empty() ? "" : encodeUrlPath(decision.markdownUrl);
}

RenderOptions renderOptionsFor(const CompiledApp& app, std::string nonce) {
    RenderOptions o;
    o.nonce = std::move(nonce);
    o.markdownCss = app.hasMarkdownCss ? "/markdown.css" : kBuiltinMarkdownCssUrl;
    return o;
}

HttpResponse dispatch(const RouteDecision& decision, const CompiledApp& app, const CodeTree& code,
                      const RenderOptions& render, const FieldSource* fields) {
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
        return t ? page(200, *t, render, fields) : notFound(app, render);
    }
    if (entry.kind == RouteKind::Markdown) {
        const CompiledTemplate* t = app.templates.find(entry.source);
        return t ? markdownPage(*t, app, render, fields) : notFound(app, render);
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

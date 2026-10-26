#include "pipeline/stages.h"

namespace fourfold {

namespace {

// Overview §8, "Content Security Policy". The nonce goes after 'self'.
const char* const kCspScripts = "default-src 'self'; script-src 'self'";
const char* const kCspRest =
    "; style-src 'self' 'unsafe-inline'; "
    "img-src 'self' https: data:; connect-src 'self'; object-src 'none'; "
    "base-uri 'none'; form-action 'self'; frame-ancestors 'none'";

const char* const kPermissionsPolicy =
    "camera=(), microphone=(), geolocation=(), payment=(), usb=(), "
    "serial=(), bluetooth=(), display-capture=()";

HttpResponse withBody(int status, const char* contentType, std::string body) {
    HttpResponse r;
    r.status = status;
    r.headers.set("Content-Type", contentType);
    r.body = std::move(body);
    return r;
}

}  // namespace

HttpResponse errorResponse(const ScriptError& error, const CompiledApp& app, const EngineOptions& options,
                           const RenderOptions& render) {
    if (options.observer) options.observer->scriptError(error);
    if (options.showErrorDetails && options.hooks) return options.hooks->scriptErrorPage(error);
    if (options.showErrorDetails) {
        // Plain text, so nothing in the message can become markup.
        std::string where = error.file.empty() ? "a script" : error.file;
        if (error.line > 0) where += " line " + std::to_string(error.line);
        return withBody(500, "text/plain; charset=utf-8",
                        "Error in " + where + ":\n\n" + error.message +
                            "\n\n(Shown because this is dev mode. In production, visitors see the 500 page.)\n");
    }
    if (app.errorPage) {
        if (const CompiledTemplate* t = app.templates.find(*app.errorPage)) {
            RenderOptions quiet = render;
            quiet.warnings = false;
            return withBody(500, "text/html; charset=utf-8", fourfold::render(*t, EmptyFields(), quiet).html.str());
        }
    }
    return HttpResponse::text(500, "Internal Server Error\n");
}

namespace {

// Fragments go into a page that already has its nonce; a <script> in one
// would be refused by the page's CSP, so it's an error rather than a
// silently broken page (overview §8, Content Security Policy).
ScriptError fragmentScriptError(const RouteEntry& entry) {
    return {"this response is a fragment with a <script> in it, which won't run when it's swapped into a page "
            "(it can't have the page's CSP nonce); put the script in the page, or in a .js file",
            "code/" + entry.source.str(), 0};
}

}  // namespace

HttpResponse respond(const HandlerOutcome& outcome, const RouteEntry& entry, const CompiledApp& app,
                     const EngineOptions& options, const RenderOptions& render, bool htmx) {
    switch (outcome.kind) {
        case HandlerOutcome::Kind::Render:
            return withBody(200, "text/html; charset=utf-8", outcome.body);
        case HandlerOutcome::Kind::Fragment:
            if (outcome.hasScript) return errorResponse(fragmentScriptError(entry), app, options, render);
            return withBody(200, "text/html; charset=utf-8", outcome.body);
        case HandlerOutcome::Kind::Text:
            return withBody(200, "text/plain; charset=utf-8", outcome.body);
        case HandlerOutcome::Kind::Redirect: {
            if (htmx) {
                // HTMX follows a 303 itself and swaps in the result; this
                // tells it to load the new page instead (overview §4).
                HttpResponse r;
                r.status = 200;
                r.headers.set("HX-Redirect", outcome.location);
                return r;
            }
            HttpResponse r = HttpResponse::text(303, "See Other\n");
            r.headers.set("Location", outcome.location);
            return r;
        }
        case HandlerOutcome::Kind::Json:
            return withBody(200, "application/json", outcome.body);
        case HandlerOutcome::Kind::Status: {
            if (outcome.hasScript) return errorResponse(fragmentScriptError(entry), app, options, render);
            HttpResponse r;
            r.status = outcome.status;
            if (!outcome.body.empty()) r.headers.set("Content-Type", outcome.contentType);
            r.body = outcome.body;
            return r;
        }
        case HandlerOutcome::Kind::NoContent: {
            HttpResponse r;
            r.status = 204;
            return r;
        }
        case HandlerOutcome::Kind::Error:
            return errorResponse(outcome.error, app, options, render);
    }
    return HttpResponse::text(500, "Internal Server Error\n");
}

bool isHtmxRequest(const HttpRequest& request) {
    const std::string* h = request.headers.get("HX-Request");
    return h && *h == "true";
}

void addStandardHeaders(HttpResponse& response, const std::string& nonce) {
    Headers& h = response.headers;
    std::string csp = kCspScripts;
    if (!nonce.empty()) csp += " 'nonce-" + nonce + "'";
    h.set("Content-Security-Policy", csp + kCspRest);
    h.set("X-Frame-Options", "DENY");
    h.set("X-Content-Type-Options", "nosniff");
    h.set("Referrer-Policy", "strict-origin-when-cross-origin");
    h.set("Cross-Origin-Opener-Policy", "same-origin");
    h.set("Permissions-Policy", kPermissionsPolicy);
    // Dev mode: always revalidate, so edits show on reload. (Production
    // caching rules come with ETags, later.)
    if (!h.has("Cache-Control")) h.set("Cache-Control", "no-cache");
}

}  // namespace fourfold

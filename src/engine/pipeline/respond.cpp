#include "pipeline/stages.h"

namespace fourfold {

namespace {

// Overview §8, "Content Security Policy", without the script nonce for now.
const char* const kContentSecurityPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
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

HttpResponse errorResponse(const ScriptError& error, const CompiledApp& app, const EngineOptions& options) {
    if (options.onScriptError) options.onScriptError(error);
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
            return withBody(500, "text/html; charset=utf-8", renderTemplate(*t, {}));
        }
    }
    return HttpResponse::text(500, "Internal Server Error\n");
}

HttpResponse respond(const HandlerOutcome& outcome, const RouteEntry& entry, const CompiledApp& app,
                     const EngineOptions& options) {
    switch (outcome.kind) {
        case HandlerOutcome::Kind::Render: {
            const CompiledTemplate* t = app.templates.find(entry.source);
            if (!t) {
                ScriptError e{"render returned a map of fields, but code/" + entry.source.str() +
                                  " has no .html template to fill; return a string or json(...) instead",
                              "code/" + entry.source.str(), 0};
                return errorResponse(e, app, options);
            }
            return withBody(200, "text/html; charset=utf-8", renderTemplate(*t, outcome.fields));
        }
        case HandlerOutcome::Kind::Text:
            return withBody(200, "text/plain; charset=utf-8", outcome.body);
        case HandlerOutcome::Kind::Redirect: {
            HttpResponse r = HttpResponse::text(303, "See Other\n");
            r.headers.set("Location", outcome.location);
            return r;
        }
        case HandlerOutcome::Kind::Json:
            return withBody(200, "application/json", outcome.body);
        case HandlerOutcome::Kind::Status: {
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
            return errorResponse(outcome.error, app, options);
    }
    return HttpResponse::text(500, "Internal Server Error\n");
}

void addStandardHeaders(HttpResponse& response) {
    Headers& h = response.headers;
    h.set("Content-Security-Policy", kContentSecurityPolicy);
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

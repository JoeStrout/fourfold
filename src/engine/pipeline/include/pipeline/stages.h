// The request pipeline's stages (Arch §5, pipeline). Each is a function over
// plain structs, so it can be tested on its own. Engine::handle runs them in
// order: Admit, Route, Identify (the signed-in user), Gate (PolicyGate),
// Parse (query and url-encoded forms, as raw strings), Dispatch, Respond,
// and Commit (the session).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "http/message.h"
#include "observer/observer.h"
#include "partition/code_tree.h"
#include "partition/paths.h"
#include "pipeline/auth_routes.h"
#include "pipeline/hooks.h"
#include "pipeline/schema_gate.h"
#include "script/script_host.h"
#include "state/auth_store.h"
#include "state/engine_log.h"
#include "state/lock_service.h"
#include "state/rate_store.h"
#include "state/session_store.h"
#include "base/clock.h"

namespace fourfold {

struct EngineOptions {
    // Host names (without port) the app answers to; any other Host header is
    // refused. Empty accepts any. Dev mode lists localhost's names, which
    // stops DNS-rebinding attacks on the dev server from a web page.
    std::vector<std::string> allowedHosts;
    // Script errors are shown in the browser, with file and line, instead of
    // the generic 500 page. Dev mode sets this.
    bool showErrorDetails = false;
    // Hears about every script error (dev mode prints them; the engine log
    // arrives in M5). May be null. Must outlive the engine.
    Observer* observer = nullptr;
    // The dev tools' part in requests: /__dev/ routes, the injected client
    // script, and the error page. Null in production. Must outlive the engine.
    EngineHooks* hooks = nullptr;

    // The app's state (M5). Each may be null, which turns its feature off: a
    // route that uses the session then runs with none (and can't keep one).
    // All must outlive the engine.
    SessionStore* sessions = nullptr;
    LockService* sessionLocks = nullptr;   // required with `sessions`
    DataFiles* data = nullptr;
    const Dropbox* dropbox = nullptr;
    // The engine log: script errors and refused requests. May be null.
    EngineLog* log = nullptr;
    // Users (M6): who's signed in, for access rules, `req.user` and the
    // `users` module. Needs `sessions`; without it, every visitor is
    // anonymous, and restricted routes are refused. Must outlive the engine.
    AuthStore* auth = nullptr;
    // The sign-in pages under /__auth/; null: not found. Must outlive the
    // engine.
    AuthRoutes* authRoutes = nullptr;
    // Rate-limit counters and used puzzles (M7). Null: nothing is counted
    // (and a puzzle can be used more than once). Must outlive the engine.
    RateStore* rates = nullptr;
    // The time, for the timing check and puzzles. Must outlive the engine.
    const Clock* clock = &systemClock();
    // How request bodies are read (overview §8, Connection and request
    // limits). A route with file fields may take its uploads on top.
    size_t maxFormBytes = 1024 * 1024;

    // The site is reached over HTTPS (perhaps through a proxy that ends
    // TLS): cookies get Secure and the __Host- prefix, and a request's own
    // origin is https://<Host>. Off for dev mode's plain HTTP on localhost.
    bool https = false;
    // The app's database (the `db` module): the app's queries run on it.
    // Null: none. Must outlive the engine.
    QueryRunner* db = nullptr;
    // The schema lock: every request holds it shared, so migrations can
    // wait for them and hold new ones back (overview §3). Null: no lock
    // (an app without migrations, or tests). Must outlive the engine.
    SchemaGate* schema = nullptr;

    // The engine's secret key, for CSRF tokens (loadOrCreateEngineKey). If
    // empty, a random one is made for this engine, so tokens last only as
    // long as the process.
    std::string engineKey{};
};

// The host names `fourfold dev` answers to.
std::vector<std::string> localhostNames();

// --- Admit: check Host, and canonicalize the path. ---

struct AdmittedRequest {
    std::string method;
    UrlPath path;
    std::string query;       // after '?', as sent; empty if none
    bool hasQuery = false;
};

// Fills `out` and returns nullopt, or returns the response to send instead.
std::optional<HttpResponse> admit(const HttpRequest& request, const EngineOptions& options,
                                  AdmittedRequest& out);

// The visitor's address (overview §8, Trusting the client's address): the
// connection's, unless that's a trusted proxy, in which case the nearest
// address in X-Forwarded-For that isn't one (read from the right, since a
// visitor can put anything at the left).
std::string clientAddress(const HttpRequest& request, const std::vector<AddressRange>& trustedProxies);

// --- Route: look the path up in the compiled app's route table. ---

struct RouteDecision {
    enum class Kind {
        Found,
        Redirect,           // to the route's canonical URL (trailing slash or not)
        NotFound,
        MethodNotAllowed,
    };
    Kind kind = Kind::NotFound;
    const RouteEntry* entry = nullptr;     // when Found
    const HandlerRef* handler = nullptr;   // when Found, if script code handles it
    std::string location;                  // when Redirect
    std::string allow;                     // when MethodNotAllowed: "GET, HEAD, POST"
    // When Found, for a Markdown page: the response is its Markdown (the
    // request was for /about.md, not /about).
    bool markdown = false;
    // When Found, for a Markdown page: its Markdown URL ("/about.md").
    std::string markdownUrl;
};


RouteDecision route(const AdmittedRequest& request, const CompiledApp& app);

// --- Parse: what a handler sees of the request. ---

// "a=1&b=x+y" → {{"a","1"}, {"b","x y"}}. Percent escapes and '+' are decoded;
// a malformed escape is kept as is, and invalid UTF-8 becomes U+FFFD, since
// handlers get these as MiniScript strings.
RequestFacts::Pairs parseUrlEncoded(std::string_view text);

struct Submission;

// The facts a handler's `req` is built from (overview §4, The request
// object): the method, path, query, headers, and the submitted form's
// fields (from readSubmission; null for none), as raw strings. The CSRF
// token (the X-CSRF-Token header, else the _csrf field) is returned in
// `csrfToken` if asked for.
RequestFacts requestFacts(const HttpRequest& request, const AdmittedRequest& admitted,
                          const Submission* submission = nullptr, std::optional<std::string>* csrfToken = nullptr);

// --- Reserved: the engine's own files, under /__fourfold/. ---

// The response for a /__fourfold/ path, or nullopt if there's no such file:
// /__fourfold/htmx.js (htmx, with Fourfold's settings) and
// /__fourfold/markdown.css (the default Markdown stylesheet).
std::optional<HttpResponse> builtinAsset(const std::string& path);

extern const char* const kBuiltinMarkdownCssUrl;   // "/__fourfold/markdown.css"
extern const char* const kChallengeScriptUrl;      // "/__fourfold/challenge.js"

// What every template rendered for one request uses: a fresh CSP nonce, and
// the stylesheet for pages with Markdown (the app's pages/markdown.css if it
// has one, else the built-in one).
RenderOptions renderOptionsFor(const CompiledApp& app, std::string nonce);

// The render options adjusted for the route found: Markdown output for a
// /x.md request, and the rel="alternate" link for a Markdown page's HTML.
void setRenderFormat(RenderOptions& render, const RouteDecision& decision);

// --- Dispatch: produce the response content without running code. ---

// Static files, pages with no code, Markdown pages (as HTML or Markdown, by
// render.format), redirects and errors.
// Not for routes with a handler (decision.handler set), which go to the
// ScriptHost.
// `fields` fills a page template's fields (a code-less page's {{flash}});
// null leaves them empty. A page (RouteKind::Page) never reads `code`.
HttpResponse dispatch(const RouteDecision& decision, const CompiledApp& app, const CodeTree& code,
                      const RenderOptions& render, const FieldSource* fields = nullptr);

// --- Respond: turn results into responses. ---

// A handler's outcome as a response. `entry` is the route it ran for.
// `htmx`: HTMX made the request, so a redirect takes HTMX's form.
HttpResponse respond(const HandlerOutcome& outcome, const RouteEntry& entry, const CompiledApp& app,
                     const EngineOptions& options, const RenderOptions& render, bool htmx);

// A 500 response for a script error: the dev tools' error page if
// options.showErrorDetails, else pages/500.html or a generic message.
HttpResponse errorResponse(const ScriptError& error, const CompiledApp& app, const EngineOptions& options,
                           const RenderOptions& render);

// The HTMX request header (HX-Request: true).
bool isHtmxRequest(const HttpRequest& request);

// Security headers (overview §8, "Security headers by default") and cache
// rules, with `nonce` in the CSP's script-src: only <script> elements the
// engine rendered (which all carry it) run, and same-origin script files.
void addStandardHeaders(HttpResponse& response, const std::string& nonce);

}  // namespace fourfold

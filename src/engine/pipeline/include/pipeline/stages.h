// The request pipeline's stages (Arch §5, pipeline). Each is a function over
// plain structs, so it can be tested on its own. Engine::handle runs them in
// order. M2 has Admit, Route, Parse (query and url-encoded forms, as raw
// strings), Dispatch, and Respond; Identify, Gate and Commit arrive with
// their milestones.
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
#include "pipeline/hooks.h"
#include "script/script_host.h"

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
};

RouteDecision route(const AdmittedRequest& request, const CompiledApp& app);

// --- Parse: what a handler sees of the request. ---

// "a=1&b=x+y" → {{"a","1"}, {"b","x y"}}. Percent escapes and '+' are decoded;
// a malformed escape is kept as is, and invalid UTF-8 becomes U+FFFD, since
// handlers get these as MiniScript strings.
RequestFacts::Pairs parseUrlEncoded(std::string_view text);

// The facts a handler's `req` is built from (overview §4, The request
// object): the method, path, query, headers, and an url-encoded POST body.
RequestFacts requestFacts(const HttpRequest& request, const AdmittedRequest& admitted);

// --- Dispatch: produce the response content without running code. ---

// Static files, pages with no code, redirects and errors. Not for routes with
// a handler (decision.handler set), which go to the ScriptHost.
HttpResponse dispatch(const RouteDecision& decision, const CompiledApp& app, const CodeTree& code);

// --- Respond: turn results into responses. ---

// A handler's outcome as a response. `entry` is the route it ran for (a
// render fills that page's template).
HttpResponse respond(const HandlerOutcome& outcome, const RouteEntry& entry, const CompiledApp& app,
                     const EngineOptions& options);

// A 500 response for a script error: the details as plain text if
// options.showErrorDetails, else pages/500.html or a generic message.
HttpResponse errorResponse(const ScriptError& error, const CompiledApp& app, const EngineOptions& options);

// Security headers (overview §8, "Security headers by default") and cache
// rules. The CSP has no script nonce until templates add one (M4), so inline
// scripts in pages are blocked, as the design intends for unnonced scripts.
void addStandardHeaders(HttpResponse& response);

}  // namespace fourfold

// The request pipeline's stages (Arch §5, pipeline). Each is a function over
// plain structs, so it can be tested on its own. Engine::handle runs them in
// order. M1 has Admit, Route, Dispatch (static files and pages as is), and
// Respond; Identify, Gate, Parse and Commit arrive with their milestones.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "http/message.h"
#include "image/app_image.h"
#include "partition/code_tree.h"
#include "partition/paths.h"

namespace fourfold {

struct EngineOptions {
    // Host names (without port) the app answers to; any other Host header is
    // refused. Empty accepts any. Dev mode lists localhost's names, which
    // stops DNS-rebinding attacks on the dev server from a web page.
    std::vector<std::string> allowedHosts;
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

// --- Route: look the path up in the image's route table. ---

struct RouteDecision {
    enum class Kind {
        Found,
        Redirect,           // to the route's canonical URL (trailing slash or not)
        NotFound,
        MethodNotAllowed,
    };
    Kind kind = Kind::NotFound;
    const RouteEntry* entry = nullptr;   // when Found
    std::string location;                // when Redirect
};

RouteDecision route(const AdmittedRequest& request, const AppImage& image);

// --- Dispatch: produce the response content. ---

HttpResponse dispatch(const RouteDecision& decision, const AppImage& image, const CodeTree& code);

// --- Respond: headers every response gets. ---

// Security headers (overview §8, "Security headers by default") and cache
// rules. The CSP has no script nonce until templates add one (M4), so inline
// scripts in pages are blocked, as the design intends for unnonced scripts.
void addStandardHeaders(HttpResponse& response);

}  // namespace fourfold

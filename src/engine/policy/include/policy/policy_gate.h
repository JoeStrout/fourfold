// PolicyGate: decides whether a request may reach a handler (Arch §5 policy;
// overview §5). So far, CSRF protection; access rules, rate limits and the
// spam checks join it in M6 and M7. Each check can only add a denial.
//
// CSRF (overview §5, CSRF protection), for every method but GET, HEAD and
// OPTIONS:
// - the Origin header, if sent, must be this site's own origin ("null", as
//   sent from sandboxed frames and some redirects, is refused);
// - the request must carry a token, in the form field _csrf or the header
//   X-CSRF-Token, that matches the visitor's CSRF cookie.
// The token is a keyed hash of the cookie, so a site that can't read the
// cookie (which is HttpOnly, and same-site) can't make one. The engine puts
// tokens into every <form method="post"> and into HTMX's headers when it
// renders a page, so app code never sees any of this.
//
// The CSRF cookie is separate from the session's, so a visitor who never
// writes to `session` still gets no session record.
#pragma once

#include <optional>
#include <string>

#include "compiledapp/compiled_app.h"
#include "script/script_host.h"

namespace fourfold {

// What the gate looks at, from the request.
struct GateFacts {
    std::string method;
    std::optional<std::string> origin;       // the Origin header
    std::string ownOrigin;                   // "https://example.com" (scheme, Host header)
    std::optional<std::string> csrfCookie;   // the CSRF cookie's value
    std::optional<std::string> csrfToken;    // from _csrf or X-CSRF-Token
};

struct GateDecision {
    std::optional<GateApproval> approval;   // set if allowed
    int status = 0;                         // if denied: 403
    std::string reason;                     // if denied: why, for the log and dev mode
};

class PolicyGate {
public:
    // `key`: the engine's secret key (loadOrCreateEngineKey), 32 bytes.
    explicit PolicyGate(std::string key);

    GateDecision check(const RouteEntry& route, const GateFacts& facts) const;

    // A new CSRF cookie value (random).
    static std::string newCsrfCookie();
    // Whether `cookie` is a well-formed CSRF cookie value.
    static bool isCsrfCookie(const std::string& cookie);
    // The token that goes with a cookie.
    std::string tokenFor(const std::string& cookie) const;

    // The form field and header that carry the token.
    static const char* const kFormField;   // "_csrf"
    static const char* const kHeader;      // "X-CSRF-Token"

private:
    std::string key_;
};

}  // namespace fourfold

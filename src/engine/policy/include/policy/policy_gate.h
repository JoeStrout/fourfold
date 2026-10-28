// PolicyGate: decides whether a request may reach a route (Arch §5 policy;
// overview §5): its access level, then CSRF protection. Rate limits and the
// spam checks join it in M7. Each check is its own function, and can only
// add a denial.
//
// Access (overview §5, Access control): the route's effective level,
// computed by the compiler, against the visitor, looked up for this request.
// Every route goes through the gate: pages, handlers, Markdown pages and
// static files alike.
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
#include <vector>

#include "compiledapp/compiled_app.h"
#include "script/script_host.h"

namespace fourfold {

// Who's asking: signed in or not, and with which roles.
struct Visitor {
    bool signedIn = false;
    std::vector<std::string> roles;   // sorted
};

// What the gate looks at, from the request.
struct GateFacts {
    std::string method;
    std::optional<std::string> origin;       // the Origin header
    std::string ownOrigin;                   // "https://example.com" (scheme, Host header)
    std::optional<std::string> csrfCookie;   // the CSRF cookie's value
    std::optional<std::string> csrfToken;    // from _csrf or X-CSRF-Token
    Visitor visitor;
};

struct GateDecision {
    enum class Denial {
        None,        // allowed
        NeedLogin,   // the route needs a signed-in visitor, and this one isn't (401)
        Forbidden,   // signed in, but without a role the route needs (403)
        Csrf,        // the request failed CSRF protection (403)
    };
    std::optional<GateApproval> approval;   // set if allowed
    Denial denial = Denial::None;
    int status = 0;                         // if denied: 401 or 403
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
    // Each adds a denial to `d`, or leaves it alone.
    static void checkAccess(const RouteEntry& route, const GateFacts& facts, GateDecision& d);
    void checkCsrf(const GateFacts& facts, GateDecision& d) const;

    std::string key_;
};

}  // namespace fourfold

// PolicyGate: decides whether a request may reach a route (Arch §5 policy;
// overview §5): its access level, CSRF protection, rate limits, the timing
// check and proof of work, in that order. Each check is its own function,
// and can only add a denial; the first denial stands.
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
// renders a page, so app code never sees any of this. A POST to a webhook
// (webhook.x, overview §6) is exempt, as are its timing check and puzzle:
// its caller is another server, and its handler checks a signature instead.
//
// The CSRF cookie is separate from the session's, so a visitor who never
// writes to `session` still gets no session record.
//
// Rate limits (overview §5, Rate limits): the route's own (code/limits.ms),
// counted for every request to it, and the engine's defaults, counted for
// every method but GET, HEAD and OPTIONS. Counted per address group, user
// or session, in the RateStore, only for requests that passed access and
// CSRF. Over a limit: 429, with Retry-After.
//
// The timing check (minSeconds) and proof of work (challenge), for every
// method but GET, HEAD and OPTIONS on routes that ask for them: tokens the
// engine wrote into the form when it rendered it (formTimeToken,
// newChallenge), signed with the engine's key and bound to the visitor's
// CSRF cookie. A puzzle is good once: the RateStore remembers used ones.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "script/script_host.h"
#include "state/rate_store.h"

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

    // For rate limits: who the visitor is counted as. `addressGroup` is the
    // address (IPv4) or /64 (IPv6); `userId` and `sessionHash` are set if
    // they're signed in, or have a session.
    std::string addressGroup;
    std::optional<std::string> userId;
    std::optional<std::string> sessionHash;
    // The engine's default limits for this visitor (anonymous or signed in),
    // each with key "default".
    std::vector<RateRule> defaultLimits;
    // The timing token (_t) and the solved puzzle (_pow), if sent.
    std::optional<std::string> formTime;
    std::optional<std::string> challengeAnswer;
    // The fewest leading zero bits a puzzle's answer must have.
    int challengeBits = 16;
    int64_t nowMs = 0;
};

struct GateDecision {
    enum class Denial {
        None,        // allowed
        NeedLogin,   // the route needs a signed-in visitor, and this one isn't (401)
        Forbidden,   // signed in, but without a role the route needs (403)
        Csrf,        // the request failed CSRF protection (403)
        RateLimited, // over a rate limit (429, retryAfterSeconds)
        TooFast,     // sent sooner than minSeconds after its form was rendered (429, retryAfterSeconds)
        Timing,      // no valid timing token, or the form is older than maxAgeMinutes (403)
        Challenge,   // no solved, unused puzzle (403)
    };
    std::optional<GateApproval> approval;   // set if allowed
    Denial denial = Denial::None;
    int status = 0;                         // if denied: 401 or 403
    std::string reason;                     // if denied: why, for the log and dev mode
    int64_t retryAfterSeconds = 0;          // RateLimited, TooFast
    // The limits this request was counted against, with their counts after
    // it (for the dev tools): "5/minute per ip (code/limits.ms: /sign)".
    std::vector<std::pair<std::string, double>> counted;
};

class PolicyGate {
public:
    // `key`: the engine's secret key (loadOrCreateEngineKey), 32 bytes.
    explicit PolicyGate(std::string key);

    // Blocks on `rates` (SQLite) when the request has limits to count or a
    // puzzle to use up (see needsStore): call it on a worker then. With
    // `rates` null, nothing is counted, and a puzzle can be used again.
    GateDecision check(const RouteEntry& route, const GateFacts& facts, RateStore* rates = nullptr) const;
    // Whether check() would use the RateStore for this route and method.
    static bool needsStore(const RouteEntry& route, const std::string& method);

    // A new CSRF cookie value (random).
    static std::string newCsrfCookie();
    // Whether `cookie` is a well-formed CSRF cookie value.
    static bool isCsrfCookie(const std::string& cookie);
    // The token that goes with a cookie.
    std::string tokenFor(const std::string& cookie) const;

    // The form field and header that carry the token.
    static const char* const kFormField;   // "_csrf"
    static const char* const kHeader;      // "X-CSRF-Token"

    // The timing token for a form rendered at `nowMs` for the visitor with
    // this CSRF cookie (the _t field).
    std::string formTimeToken(const std::string& cookie, int64_t nowMs) const;
    // A new proof-of-work puzzle, for the same (the _pow field's
    // data-ff-challenge). The answer is the puzzle, "~", and a number
    // (decimal) such that SHA-256 of all of it, as text, starts with `bits`
    // zero bits.
    std::string newChallenge(const std::string& cookie, int64_t nowMs, int bits) const;
    // How long a puzzle stays good.
    static constexpr int64_t kChallengeLifetimeMs = 24 * 3600 * 1000;

private:
    // Each adds a denial to `d`, or leaves it alone.
    static void checkAccess(const RouteEntry& route, const GateFacts& facts, GateDecision& d);
    void checkCsrf(const GateFacts& facts, GateDecision& d) const;
    static void checkRates(const RouteEntry& route, const GateFacts& facts, RateStore* rates, GateDecision& d);
    void checkTiming(const RouteEntry& route, const GateFacts& facts, GateDecision& d) const;
    void checkChallenge(const RouteEntry& route, const GateFacts& facts, RateStore* rates, GateDecision& d) const;
    std::string mac(const std::string& message) const;

    std::string key_;
};

}  // namespace fourfold

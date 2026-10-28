#include "policy/policy_gate.h"

#include <cctype>

#include "base/crypto.h"
#include "platform/random.h"

namespace fourfold {

namespace {

constexpr size_t kCookieBytes = 32;

void deny(GateDecision& d, GateDecision::Denial denial, int status, std::string reason) {
    d.denial = denial;
    d.status = status;
    d.reason = std::move(reason);
}

bool isSafeMethod(const std::string& method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

// Origins compare as ASCII, case-insensitively, after dropping a default
// port; that's all browsers vary in.
std::string normalizeOrigin(std::string origin) {
    for (char& c : origin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto dropSuffix = [&](const char* scheme, const char* port) {
        std::string s = scheme, p = port;
        if (origin.compare(0, s.size(), s) == 0 && origin.size() > p.size() &&
            origin.compare(origin.size() - p.size(), p.size(), p) == 0) {
            origin.resize(origin.size() - p.size());
        }
    };
    dropSuffix("http://", ":80");
    dropSuffix("https://", ":443");
    return origin;
}

}  // namespace

const char* const PolicyGate::kFormField = "_csrf";
const char* const PolicyGate::kHeader = "X-CSRF-Token";

PolicyGate::PolicyGate(std::string key) : key_(std::move(key)) {}

std::string PolicyGate::newCsrfCookie() { return toBase64Url(randomBytes(kCookieBytes)); }

bool PolicyGate::isCsrfCookie(const std::string& cookie) {
    auto raw = fromBase64Url(cookie);
    return raw && raw->size() == kCookieBytes;
}

std::string PolicyGate::tokenFor(const std::string& cookie) const {
    return toBase64Url(keyedHash(key_, "csrf:" + cookie));
}

void PolicyGate::checkAccess(const RouteEntry& route, const GateFacts& facts, GateDecision& d) {
    const AccessLevel& access = route.access;
    if (access.allows(facts.visitor.signedIn, facts.visitor.roles)) return;
    if (!facts.visitor.signedIn) {
        deny(d, GateDecision::Denial::NeedLogin, 401, "this page is for signed-in visitors (" + access.describe() + ")");
    } else {
        deny(d, GateDecision::Denial::Forbidden, 403, "this page needs " + access.describe() + "; the visitor has " +
                                                         (facts.visitor.roles.empty() ? std::string("no roles")
                                                                                      : "other roles"));
    }
}

void PolicyGate::checkCsrf(const GateFacts& facts, GateDecision& d) const {
    if (isSafeMethod(facts.method)) return;
    auto refuse = [&](std::string reason) { deny(d, GateDecision::Denial::Csrf, 403, std::move(reason)); };
    if (facts.origin) {
        if (*facts.origin == "null") return refuse("the request came from an opaque origin (Origin: null)");
        if (normalizeOrigin(*facts.origin) != normalizeOrigin(facts.ownOrigin)) {
            return refuse("the request came from another site (Origin: " + *facts.origin + ")");
        }
    }
    if (!facts.csrfCookie || !isCsrfCookie(*facts.csrfCookie)) {
        return refuse("no CSRF cookie: the form wasn't loaded from this site (or cookies are off)");
    }
    if (!facts.csrfToken) {
        return refuse("no CSRF token: forms must be in a page the engine rendered, and HTMX requests need the "
                      "page's hx-headers");
    }
    if (!constantTimeEqual(*facts.csrfToken, tokenFor(*facts.csrfCookie))) {
        return refuse("the CSRF token doesn't match (the page may be older than the visitor's cookie)");
    }
}

GateDecision PolicyGate::check(const RouteEntry& route, const GateFacts& facts) const {
    GateDecision d;
    checkAccess(route, facts, d);
    if (d.denial == GateDecision::Denial::None) checkCsrf(facts, d);
    if (d.denial == GateDecision::Denial::None) d.approval = GateApproval();
    return d;
}

}  // namespace fourfold

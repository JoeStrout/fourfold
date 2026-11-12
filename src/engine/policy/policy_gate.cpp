#include "policy/policy_gate.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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

std::string PolicyGate::mac(const std::string& message) const {
    return toBase64Url(keyedHash(key_, message).substr(0, 16));
}

std::string PolicyGate::formTimeToken(const std::string& cookie, int64_t nowMs) const {
    std::string ms = std::to_string(nowMs);
    return ms + "." + mac("t:" + ms + ":" + cookie);
}

std::string PolicyGate::newChallenge(const std::string& cookie, int64_t nowMs, int bits) const {
    std::string body = toBase64Url(randomBytes(12)) + "." + std::to_string(nowMs) + "." + std::to_string(bits);
    return body + "." + mac("pow:" + body + ":" + cookie);
}

namespace {

// "123" → 123; nullopt if it isn't a plain non-negative decimal.
std::optional<int64_t> parseDecimal(const std::string& s, size_t maxDigits = 15) {
    if (s.empty() || s.size() > maxDigits) return std::nullopt;
    int64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + (c - '0');
    }
    return n;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

std::string subjectFor(const RateRule& rule, const GateFacts& facts) {
    if (rule.per == RateRule::Per::User && facts.userId) return "user:" + *facts.userId;
    if (rule.per == RateRule::Per::Session && facts.sessionHash) return "session:" + toHex(*facts.sessionHash);
    return "ip:" + facts.addressGroup;
}

}  // namespace

bool PolicyGate::needsStore(const RouteEntry& route, const std::string& method) {
    return !route.limits.empty() || !isSafeMethod(method);
}

void PolicyGate::checkRates(const RouteEntry& route, const GateFacts& facts, RateStore* rates, GateDecision& d) {
    std::vector<const RateRule*> rules;
    for (const RateRule& r : route.limits) rules.push_back(&r);
    if (!isSafeMethod(facts.method)) {
        for (const RateRule& r : facts.defaultLimits) rules.push_back(&r);
    }
    if (rules.empty() || !rates) return;
    std::vector<RateCheck> checks;
    for (const RateRule* r : rules) checks.push_back({r->key + "|" + subjectFor(*r, facts), r->count, r->seconds});
    auto result = rates->hit(checks);
    if (!result) return;   // a store failure doesn't refuse the request (the error is the operator's)
    for (size_t i = 0; i < result.value().counts.size() && i < rules.size(); i++) {
        d.counted.emplace_back(rules[i]->describe() + " (" + rules[i]->key + ")", result.value().counts[i]);
    }
    if (!result.value().allowed) {
        const RateRule& r = *rules[result.value().refusedIndex];
        deny(d, GateDecision::Denial::RateLimited, 429, "over the limit of " + r.describe() + " (" + r.key + ")");
        d.retryAfterSeconds = result.value().retryAfterSeconds;
    }
}

void PolicyGate::checkTiming(const RouteEntry& route, const GateFacts& facts, GateDecision& d) const {
    if (isSafeMethod(facts.method) || route.minSeconds <= 0) return;
    auto refuse = [&](std::string reason) { deny(d, GateDecision::Denial::Timing, 403, std::move(reason)); };
    if (!facts.formTime || !facts.csrfCookie) {
        return refuse("no timing token (_t): this route has minSeconds, so the form must come from a page the "
                      "engine rendered");
    }
    std::vector<std::string> parts = split(*facts.formTime, '.');
    std::optional<int64_t> ms = parts.size() == 2 ? parseDecimal(parts[0]) : std::nullopt;
    if (!ms || !constantTimeEqual(parts[1], mac("t:" + parts[0] + ":" + *facts.csrfCookie))) {
        return refuse("the timing token (_t) isn't valid");
    }
    double age = static_cast<double>(facts.nowMs - *ms) / 1000.0;
    if (age < route.minSeconds) {
        deny(d, GateDecision::Denial::TooFast, 429,
             "sent " + std::to_string(static_cast<int64_t>(age * 1000)) + " ms after the form was rendered; "
             "this route needs at least " + std::to_string(static_cast<int64_t>(route.minSeconds)) + " s");
        d.retryAfterSeconds = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(route.minSeconds - age)));
        return;
    }
    if (route.maxAgeSeconds > 0 && age > route.maxAgeSeconds) {
        return refuse("the form is older than this route's maxAgeMinutes; reload it");
    }
}

void PolicyGate::checkChallenge(const RouteEntry& route, const GateFacts& facts, RateStore* rates,
                                GateDecision& d) const {
    if (isSafeMethod(facts.method) || !route.challenge) return;
    auto refuse = [&](std::string reason) { deny(d, GateDecision::Denial::Challenge, 403, std::move(reason)); };
    if (!facts.challengeAnswer || facts.challengeAnswer->empty() || !facts.csrfCookie) {
        return refuse("no proof of work (_pow): this form needs JavaScript to solve its puzzle before it's sent");
    }
    size_t tilde = facts.challengeAnswer->rfind('~');
    if (tilde == std::string::npos) return refuse("the proof of work (_pow) is malformed");
    std::string puzzle = facts.challengeAnswer->substr(0, tilde);
    std::string answer = facts.challengeAnswer->substr(tilde + 1);
    std::vector<std::string> parts = split(puzzle, '.');
    if (parts.size() != 4 || !parseDecimal(answer)) return refuse("the proof of work (_pow) is malformed");
    std::string body = parts[0] + "." + parts[1] + "." + parts[2];
    std::optional<int64_t> issued = parseDecimal(parts[1]);
    std::optional<int64_t> bits = parseDecimal(parts[2], 3);
    if (!issued || !bits || !constantTimeEqual(parts[3], mac("pow:" + body + ":" + *facts.csrfCookie))) {
        return refuse("the proof of work's puzzle isn't one the engine made for this visitor");
    }
    if (facts.nowMs - *issued > kChallengeLifetimeMs) return refuse("the puzzle has expired; reload the form");
    if (*bits < facts.challengeBits) return refuse("the puzzle is easier than this site now asks for; reload the form");
    if (leadingZeroBits(sha256(*facts.challengeAnswer)) < *bits) return refuse("the puzzle's answer is wrong");
    if (rates) {
        auto fresh = rates->useOnce(parts[0], *issued + kChallengeLifetimeMs);
        if (fresh && !fresh.value()) return refuse("this puzzle has been used already; reload the form");
    }
}

GateDecision PolicyGate::check(const RouteEntry& route, const GateFacts& facts, RateStore* rates) const {
    GateDecision d;
    auto open = [&] { return d.denial == GateDecision::Denial::None; };
    // A webhook (overview §6) is called by another server, not from a page
    // the engine rendered, so it has no CSRF token, timing token or puzzle;
    // its handler checks the caller's signature instead. Access and rate
    // limits still apply.
    bool webhook = route.webhook && facts.method == "POST";
    checkAccess(route, facts, d);
    if (open() && !webhook) checkCsrf(facts, d);
    if (open()) checkRates(route, facts, rates, d);
    if (open() && !webhook) checkTiming(route, facts, d);
    if (open() && !webhook) checkChallenge(route, facts, rates, d);
    if (open()) d.approval = GateApproval();
    return d;
}

}  // namespace fourfold

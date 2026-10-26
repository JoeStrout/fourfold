#include "policy/policy_gate.h"

#include "doctest.h"

using namespace fourfold;

namespace {

const std::string kKey(32, 'k');

GateFacts post(std::optional<std::string> origin, std::optional<std::string> cookie, std::optional<std::string> token) {
    GateFacts f;
    f.method = "POST";
    f.origin = std::move(origin);
    f.ownOrigin = "https://example.com";
    f.csrfCookie = std::move(cookie);
    f.csrfToken = std::move(token);
    return f;
}

}  // namespace

TEST_CASE("PolicyGate: CSRF, table-driven") {
    PolicyGate gate(kKey);
    RouteEntry route{RouteKind::Handler, RelPath::parse("pages/x.ms").value()};
    std::string cookie = PolicyGate::newCsrfCookie();
    std::string other = PolicyGate::newCsrfCookie();
    std::string token = gate.tokenFor(cookie);
    CHECK_NE(token, gate.tokenFor(other));
    CHECK_NE(token, PolicyGate(std::string(32, 'j')).tokenFor(cookie));   // depends on the key

    struct Case {
        const char* name;
        GateFacts facts;
        bool allowed;
    };
    const Case cases[] = {
        {"valid, no Origin", post(std::nullopt, cookie, token), true},
        {"valid, same Origin", post("https://example.com", cookie, token), true},
        {"same Origin with default port", post("https://EXAMPLE.com:443", cookie, token), true},
        {"other Origin", post("https://evil.example", cookie, token), false},
        {"other scheme", post("http://example.com", cookie, token), false},
        {"other port", post("https://example.com:8443", cookie, token), false},
        {"lookalike suffix", post("https://example.com.evil.example", cookie, token), false},
        {"null Origin", post("null", cookie, token), false},
        {"no cookie", post(std::nullopt, std::nullopt, token), false},
        {"malformed cookie", post(std::nullopt, std::string("abc"), token), false},
        {"no token", post(std::nullopt, cookie, std::nullopt), false},
        {"empty token", post(std::nullopt, cookie, std::string()), false},
        {"another cookie's token", post(std::nullopt, cookie, gate.tokenFor(other)), false},
        {"the cookie as token", post(std::nullopt, cookie, cookie), false},
        {"token off by one char", post(std::nullopt, cookie, token.substr(0, token.size() - 1) + "A"), false},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        GateDecision d = gate.check(route, c.facts);
        CHECK_EQ(d.approval.has_value(), c.allowed);
        if (!c.allowed) {
            CHECK_EQ(d.status, 403);
            CHECK_FALSE(d.reason.empty());
        }
    }
    for (const char* method : {"GET", "HEAD", "OPTIONS"}) {
        GateFacts f = post("https://evil.example", std::nullopt, std::nullopt);
        f.method = method;
        CHECK(gate.check(route, f).approval);
    }
    for (const char* method : {"PUT", "DELETE", "PATCH"}) {
        GateFacts f = post(std::nullopt, std::nullopt, std::nullopt);
        f.method = method;
        CHECK_FALSE(gate.check(route, f).approval);
    }
}

TEST_CASE("PolicyGate: CSRF cookies") {
    std::string c = PolicyGate::newCsrfCookie();
    CHECK(PolicyGate::isCsrfCookie(c));
    CHECK_NE(c, PolicyGate::newCsrfCookie());
    CHECK_FALSE(PolicyGate::isCsrfCookie(""));
    CHECK_FALSE(PolicyGate::isCsrfCookie(c.substr(1)));
    CHECK_FALSE(PolicyGate::isCsrfCookie(c + "x"));
}

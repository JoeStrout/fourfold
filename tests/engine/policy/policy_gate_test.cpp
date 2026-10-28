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
        {"token off by one char",
         post(std::nullopt, cookie, token.substr(0, token.size() - 1) + (token.back() == 'A' ? "B" : "A")), false},
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

TEST_CASE("PolicyGate: access, every level against every kind of visitor and method") {
    PolicyGate gate(kKey);
    std::string cookie = PolicyGate::newCsrfCookie();
    std::string token = gate.tokenFor(cookie);

    auto level = [](std::vector<std::vector<std::string>> reqs) {
        AccessLevel a;
        for (auto& r : reqs) a.require(r);
        return a;
    };
    struct Level {
        const char* name;
        AccessLevel access;
    };
    const Level levels[] = {
        {"public", level({})},
        {"login", level({{}})},
        {"admin", level({{"admin"}})},
        {"admin or moderator", level({{"admin", "moderator"}})},
        {"editor and admin", level({{"editor"}, {"admin"}})},
    };
    struct Who {
        const char* name;
        Visitor visitor;
    };
    const Who visitors[] = {
        {"anonymous", {false, {}}},
        {"member", {true, {}}},
        {"moderator", {true, {"moderator"}}},
        {"admin", {true, {"admin"}}},
        {"admin+editor", {true, {"admin", "editor"}}},
    };
    // Expected: allowed? by [level][visitor]
    const bool expected[5][5] = {
        {true, true, true, true, true},
        {false, true, true, true, true},
        {false, false, false, true, true},
        {false, false, true, true, true},
        {false, false, false, false, true},
    };
    for (size_t l = 0; l < 5; l++) {
        RouteEntry route{RouteKind::Static, RelPath::parse("pages/x.png").value()};
        route.access = levels[l].access;
        for (size_t v = 0; v < 5; v++) {
            for (const char* method : {"GET", "HEAD", "POST", "DELETE"}) {
                INFO(levels[l].name << " / " << visitors[v].name << " / " << method);
                GateFacts f = post(std::nullopt, cookie, token);
                f.method = method;
                f.visitor = visitors[v].visitor;
                GateDecision d = gate.check(route, f);
                CHECK_EQ(d.approval.has_value(), expected[l][v]);
                if (expected[l][v]) continue;
                bool signedIn = visitors[v].visitor.signedIn;
                CHECK(d.denial == (signedIn ? GateDecision::Denial::Forbidden : GateDecision::Denial::NeedLogin));
                CHECK_EQ(d.status, signedIn ? 403 : 401);
            }
        }
    }
}

TEST_CASE("PolicyGate: access is checked before CSRF, and both must pass") {
    PolicyGate gate(kKey);
    RouteEntry route{RouteKind::Handler, RelPath::parse("pages/x.ms").value()};
    route.access.require({"admin"});
    GateFacts f = post(std::nullopt, std::nullopt, std::nullopt);
    GateDecision d = gate.check(route, f);
    CHECK(d.denial == GateDecision::Denial::NeedLogin);
    f.visitor = {true, {"admin"}};
    d = gate.check(route, f);
    CHECK(d.denial == GateDecision::Denial::Csrf);
    CHECK_FALSE(d.approval);
}

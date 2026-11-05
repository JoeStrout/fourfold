#include "policy/policy_gate.h"

#include "base/crypto.h"
#include "doctest.h"
#include "temp_dir.h"

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

namespace {

struct Limited {
    fourfold::test::TempDir dir;
    ManualClock clock{1'700'000'040'000};
    std::unique_ptr<RateStore> rates = RateStore::open(dir.at("state.db"), clock).value();
    PolicyGate gate{kKey};
    std::string cookie = PolicyGate::newCsrfCookie();

    GateFacts facts(const char* method = "POST", const char* ip = "198.51.100.1") {
        GateFacts f = post(std::nullopt, cookie, gate.tokenFor(cookie));
        f.method = method;
        f.addressGroup = ip;
        f.nowMs = clock.nowMillis();
        return f;
    }
};

RateRule rate(const char* text, const char* key) {
    std::string error;
    RateRule r = *parseRate(text, error);
    r.key = key;
    return r;
}

// Solves a puzzle the way the browser's solver does.
std::string solve(const std::string& puzzle) {
    for (int n = 0;; n++) {
        std::string answer = puzzle + "~" + std::to_string(n);
        int bits = std::stoi(puzzle.substr(puzzle.find('.', puzzle.find('.') + 1) + 1));
        if (leadingZeroBits(sha256(answer)) >= bits) return answer;
    }
}

}  // namespace

TEST_CASE("parseRate: the forms a rate is written in") {
    std::string error;
    struct Case {
        const char* text;
        int64_t count, seconds;
        RateRule::Per per;
    };
    const Case cases[] = {
        {"5/minute", 5, 60, RateRule::Per::Ip},
        {"100/minute per user", 100, 60, RateRule::Per::User},
        {"100 per minute per user", 100, 60, RateRule::Per::User},
        {"100/minute/user", 100, 60, RateRule::Per::User},
        {"10/hours per session", 10, 3600, RateRule::Per::Session},
        {"  50 / Day  ", 50, 86400, RateRule::Per::Ip},
        {"1/second per ip", 1, 1, RateRule::Per::Ip},
    };
    for (const Case& c : cases) {
        CAPTURE(c.text);
        auto r = parseRate(c.text, error);
        REQUIRE(r);
        CHECK_EQ(r->count, c.count);
        CHECK_EQ(r->seconds, c.seconds);
        CHECK(r->per == c.per);
    }
    for (const char* bad : {"", "5", "five/minute", "0/minute", "5/fortnight", "5/minute per cat", "5/minute/user/x",
                            "-1/minute", "5 minute"}) {
        CAPTURE(bad);
        CHECK_FALSE(parseRate(bad, error));
        CHECK_FALSE(error.empty());
    }
    CHECK_EQ(parseRate("5/minute per user", error)->describe(), "5/minute per user");
}

TEST_CASE("PolicyGate: rate limits, default and per route") {
    Limited t;
    RouteEntry route{RouteKind::Handler, RelPath::parse("pages/x.ms").value()};
    route.limits = {rate("3/minute", "code/limits.ms: /x")};

    // The route's rule counts every request, GET included...
    for (int i = 0; i < 3; i++) CHECK(t.gate.check(route, t.facts("GET"), t.rates.get()).approval);
    GateDecision d = t.gate.check(route, t.facts("GET"), t.rates.get());
    CHECK(d.denial == GateDecision::Denial::RateLimited);
    CHECK_EQ(d.status, 429);
    CHECK_GE(d.retryAfterSeconds, 1);
    // ...per address group: another visitor has a count of their own.
    CHECK(t.gate.check(route, t.facts("GET", "203.0.113.5"), t.rates.get()).approval);

    // The defaults count only methods other than GET.
    RouteEntry plain{RouteKind::Handler, RelPath::parse("pages/y.ms").value()};
    GateFacts post2 = t.facts("POST", "192.0.2.9");
    post2.defaultLimits = {rate("2/minute", "default")};
    CHECK(t.gate.check(plain, post2, t.rates.get()).approval);
    CHECK(t.gate.check(plain, post2, t.rates.get()).approval);
    CHECK(t.gate.check(plain, post2, t.rates.get()).denial == GateDecision::Denial::RateLimited);
    GateFacts get2 = post2;
    get2.method = "GET";
    for (int i = 0; i < 5; i++) CHECK(t.gate.check(plain, get2, t.rates.get()).approval);
    CHECK_FALSE(PolicyGate::needsStore(plain, "GET"));
    CHECK(PolicyGate::needsStore(plain, "POST"));
    CHECK(PolicyGate::needsStore(route, "GET"));

    // Per user: a signed-in visitor is counted by their id, wherever they are;
    // an anonymous one falls back to their address.
    RouteEntry perUser{RouteKind::Handler, RelPath::parse("pages/z.ms").value()};
    perUser.limits = {rate("1/minute per user", "code/limits.ms: /z")};
    GateFacts ann = t.facts("GET", "10.0.0.1");
    ann.userId = "ann";
    CHECK(t.gate.check(perUser, ann, t.rates.get()).approval);
    ann.addressGroup = "10.0.0.2";
    CHECK_FALSE(t.gate.check(perUser, ann, t.rates.get()).approval);
    GateFacts bob = t.facts("GET", "10.0.0.1");
    bob.userId = "bob";
    CHECK(t.gate.check(perUser, bob, t.rates.get()).approval);

    // Refused by access or CSRF: not counted.
    RouteEntry restricted = route;
    restricted.limits = {rate("1/minute", "code/limits.ms: /r")};
    restricted.access.require({"admin"});
    for (int i = 0; i < 3; i++) CHECK(t.gate.check(restricted, t.facts("GET", "192.0.2.50"), t.rates.get()).denial ==
                                     GateDecision::Denial::NeedLogin);
    GateFacts admin = t.facts("GET", "192.0.2.50");
    admin.visitor = {true, {"admin"}};
    CHECK(t.gate.check(restricted, admin, t.rates.get()).approval);
}

TEST_CASE("PolicyGate: the timing check") {
    Limited t;
    RouteEntry route{RouteKind::Handler, RelPath::parse("pages/x.ms").value()};
    route.minSeconds = 3;
    route.maxAgeSeconds = 600;
    std::string token = t.gate.formTimeToken(t.cookie, t.clock.nowMillis());

    GateFacts f = t.facts();
    f.formTime = token;
    GateDecision tooSoon = t.gate.check(route, f);
    CHECK(tooSoon.denial == GateDecision::Denial::TooFast);
    CHECK_EQ(tooSoon.status, 429);
    CHECK_EQ(tooSoon.retryAfterSeconds, 3);

    t.clock.advanceSeconds(4);
    f = t.facts();
    f.formTime = token;
    CHECK(t.gate.check(route, f).approval);
    CHECK(t.gate.check(route, f).approval);   // not single use (overview §5, still open)

    // Missing, forged, another visitor's, or too old.
    f.formTime.reset();
    CHECK(t.gate.check(route, f).denial == GateDecision::Denial::Timing);
    std::string early = std::to_string(t.clock.nowMillis() - 3'600'000);
    f.formTime = early + token.substr(token.find('.'));   // an earlier time, the old signature
    CHECK(t.gate.check(route, f).denial == GateDecision::Denial::Timing);
    f.formTime = t.gate.formTimeToken(PolicyGate::newCsrfCookie(), t.clock.nowMillis() - 10'000);
    CHECK(t.gate.check(route, f).denial == GateDecision::Denial::Timing);
    f.formTime = t.gate.formTimeToken(t.cookie, t.clock.nowMillis() - 601'000);
    CHECK(t.gate.check(route, f).denial == GateDecision::Denial::Timing);
    // GET isn't checked.
    GateFacts get = t.facts("GET");
    CHECK(t.gate.check(route, get).approval);
}

TEST_CASE("PolicyGate: proof of work") {
    Limited t;
    RouteEntry route{RouteKind::Handler, RelPath::parse("pages/x.ms").value()};
    route.challenge = true;
    std::string puzzle = t.gate.newChallenge(t.cookie, t.clock.nowMillis(), 8);
    std::string answer = solve(puzzle);

    GateFacts f = t.facts();
    f.challengeBits = 8;
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Challenge);   // none
    f.challengeAnswer = answer;
    CHECK(t.gate.check(route, f, t.rates.get()).approval);
    // Good once.
    GateDecision again = t.gate.check(route, f, t.rates.get());
    CHECK(again.denial == GateDecision::Denial::Challenge);
    CHECK(again.reason.find("used") != std::string::npos);

    // A wrong answer; another visitor's puzzle; an easier puzzle than asked
    // for; an expired one; a forged one.
    std::string fresh = t.gate.newChallenge(t.cookie, t.clock.nowMillis(), 8);
    std::string good = solve(fresh);
    for (int n = 0;; n++) {
        std::string wrong = fresh + "~" + std::to_string(n);
        if (leadingZeroBits(sha256(wrong)) >= 8) continue;
        f.challengeAnswer = wrong;
        break;
    }
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Challenge);
    f.challengeAnswer = solve(t.gate.newChallenge(PolicyGate::newCsrfCookie(), t.clock.nowMillis(), 8));
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Challenge);
    f.challengeAnswer = solve(t.gate.newChallenge(t.cookie, t.clock.nowMillis(), 2));
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Challenge);
    f.challengeAnswer = solve(t.gate.newChallenge(t.cookie, t.clock.nowMillis() - PolicyGate::kChallengeLifetimeMs - 1, 8));
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Challenge);
    std::string forged = fresh;
    forged.replace(forged.rfind('.') + 1, 4, "AAAA");
    f.challengeAnswer = solve(forged);
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Challenge);
    for (const char* malformed : {"", "~", "x~1", "a.b.c.d~x", "a.b~1"}) {
        f.challengeAnswer = malformed;
        CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Challenge);
    }
    f.challengeAnswer = good;
    CHECK(t.gate.check(route, f, t.rates.get()).approval);
}

TEST_CASE("PolicyGate: the checks' order: access, CSRF, rates, timing, puzzle") {
    Limited t;
    RouteEntry route{RouteKind::Handler, RelPath::parse("pages/x.ms").value()};
    route.limits = {rate("100/minute", "k")};
    route.minSeconds = 1;
    route.challenge = true;
    GateFacts f = t.facts();
    f.csrfToken.reset();
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Csrf);
    f = t.facts();
    CHECK(t.gate.check(route, f, t.rates.get()).denial == GateDecision::Denial::Timing);
    f.formTime = t.gate.formTimeToken(t.cookie, t.clock.nowMillis() - 5000);
    GateDecision d = t.gate.check(route, f, t.rates.get());
    CHECK(d.denial == GateDecision::Denial::Challenge);
    CHECK_EQ(d.counted.size(), 1u);   // counted, though refused after
}

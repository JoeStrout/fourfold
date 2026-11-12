// Outbound HTTP (M9; overview §6): the address rules, URLs, the allowlist,
// secrets, redirects and limits, against a fake server on 127.0.0.1.
//
// Names are made to resolve where a test wants with the client's `resolve`
// setting (curl's --resolve), so "api.test" can be the fake server and
// "evil.test" a private address. The fake server is on loopback, which the
// address rules refuse, so tests that reach it allow 127.0.0.1 with
// allowAddresses, as an admin would allow an internal service.
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "base/crypto.h"
#include "doctest.h"
#include "fake_http.h"
#include "outbound/address_rules.h"
#include "outbound/outbound_client.h"

using namespace fourfold;
using fourfold::test::FakeHttp;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string blocked(const std::string& address) {
    auto bytes = parseAddress(address);
    REQUIRE_MESSAGE(bytes, address);
    return blockedAddressReason(*bytes);
}

OutboundPolicy policyFor(std::vector<std::string> rules) {
    OutboundPolicy p;
    p.declared = true;
    for (const std::string& r : rules) {
        std::string error;
        auto rule = parseHostRule(r, error);
        REQUIRE_MESSAGE(rule, error);
        p.hosts.push_back(*rule);
    }
    return p;
}

// Settings that let names resolve to the fake server (or elsewhere), with
// loopback allowed.
OutboundSettings settingsFor(int port, std::vector<std::string> extraResolve = {}, bool allowLoopback = true) {
    OutboundSettings s;
    for (const char* name : {"api.test", "other.test", "two.test"}) {
        s.resolve.push_back(std::string(name) + ":" + std::to_string(port) + ":127.0.0.1");
    }
    for (std::string& r : extraResolve) s.resolve.push_back(std::move(r));
    if (allowLoopback) s.allowAddresses.push_back(*AddressRange::parse("127.0.0.1"));
    s.timeout = std::chrono::milliseconds(3000);
    return s;
}

OutboundRequest get(const std::string& url) {
    OutboundRequest r;
    r.url = {url};
    return r;
}

std::string hex(const std::string& bytes) { return toHex(bytes); }

// The calls the client reported, for checking the log never has a secret.
struct Calls {
    std::mutex m;
    std::vector<OutboundCall> all;
    std::function<void(const OutboundCall&)> hook() {
        return [this](const OutboundCall& c) {
            std::lock_guard<std::mutex> lock(m);
            all.push_back(c);
        };
    }
    std::string text() {
        std::lock_guard<std::mutex> lock(m);
        std::string s;
        for (const OutboundCall& c : all) s += c.method + " " + c.url + " " + c.host + " " + c.refused + "\n";
        return s;
    }
};

constexpr auto kTime = std::chrono::milliseconds(5000);

}  // namespace

TEST_CASE("blocked addresses") {
    // Every range overview §6 names, and the IPv6 forms that carry IPv4.
    for (const char* a : {"0.0.0.0", "0.1.2.3", "10.0.0.1", "10.255.255.255", "100.64.0.1", "100.127.255.255",
                          "127.0.0.1", "127.1.2.3", "169.254.169.254", "172.16.0.1", "172.31.255.255", "192.0.0.1",
                          "192.0.2.1", "192.88.99.1", "192.168.1.1", "198.18.0.1", "198.19.255.255", "198.51.100.7",
                          "203.0.113.9", "224.0.0.1", "239.255.255.250", "240.0.0.1", "255.255.255.255", "::", "::1",
                          "::ffff:127.0.0.1", "::ffff:169.254.169.254", "::ffff:10.0.0.1", "::127.0.0.1",
                          "::169.254.169.254", "::8.8.8.8", "64:ff9b::a9fe:a9fe", "64:ff9b::7f00:1",
                          "64:ff9b:1::1", "100::1", "2002:7f00:1::1", "2002:a9fe:a9fe::1", "2001:db8::1",
                          "2001::1", "2001:0:4136:e378::1", "fc00::1", "fd12:3456::1", "fe80::1", "fec0::1",
                          "ff02::1", "3fff::1"}) {
        CHECK_MESSAGE(!blocked(a).empty(), a);
    }
    for (const char* a : {"8.8.8.8", "1.1.1.1", "93.184.216.34", "100.63.255.255", "100.128.0.1", "172.15.255.255",
                          "172.32.0.1", "192.167.1.1", "169.253.0.1", "2606:4700::1111", "2a00:1450::1",
                          "64:ff9b::808:808", "2002:808:808::1", "::ffff:8.8.8.8"}) {
        CHECK_MESSAGE(blocked(a).empty(), a, ": ", blocked(a));
    }
    CHECK(contains(blocked("169.254.169.254"), "metadata"));
    CHECK(contains(blocked("::ffff:127.0.0.1"), "loopback"));
    CHECK(contains(blocked("64:ff9b::a9fe:a9fe"), "NAT64"));

    // An admin can allow named internal addresses, and only those.
    std::vector<AddressRange> allowed = {*AddressRange::parse("10.1.2.0/24")};
    CHECK(blockedAddressReason(*parseAddress("10.1.2.3"), allowed).empty());
    CHECK(!blockedAddressReason(*parseAddress("10.1.3.3"), allowed).empty());
    CHECK(!blockedAddressReason(*parseAddress("169.254.169.254"), allowed).empty());
}

TEST_CASE("URLs") {
    auto u = parseOutboundUrl("https://API.Example.com/a b?q=x y#frag");
    REQUIRE(u);
    CHECK(u.value().host == "api.example.com");
    CHECK(u.value().https);
    CHECK(u.value().port == 443);
    CHECK(u.value().target == "/a%20b?q=x%20y");

    u = parseOutboundUrl("http://example.com:8080?x=1");
    REQUIRE(u);
    CHECK(!u.value().https);
    CHECK(u.value().port == 8080);
    CHECK(u.value().target == "/?x=1");

    u = parseOutboundUrl("https://[::1]:8443/");
    REQUIRE(u);
    CHECK(u.value().host == "::1");
    CHECK(u.value().ipLiteral);

    u = parseOutboundUrl("http://127.0.0.1/");
    REQUIRE(u);
    CHECK(u.value().ipLiteral);

    // Schemes, credentials in URLs, and numbers that only look like hosts.
    for (const char* bad : {"ftp://example.com/", "file:///etc/passwd", "example.com", "//example.com/",
                            "https://user:pw@example.com/", "https://example.com@evil.com/", "http://0x7f.1/",
                            "http://2130706433/", "http://017700000001/", "http://127.1/", "https://exa mple.com/",
                            "https://example.com:0/", "https://example.com:99999/",
                            "https://[::1/", "https://ex\\ample.com/", "https:///path", "https://-bad.com/",
                            "https://bücher.de/"}) {
        CHECK_MESSAGE(!parseOutboundUrl(bad), bad);
    }

    // The one rule redirects follow whatever the allowlist says.
    CHECK(!redirectRefusal(parseOutboundUrl("https://a.com/").value(), parseOutboundUrl("http://a.com/").value())
               .empty());
    CHECK(redirectRefusal(parseOutboundUrl("http://a.com/").value(), parseOutboundUrl("https://a.com/").value())
              .empty());
}

TEST_CASE("host rules") {
    std::string error;
    auto rule = [&](const char* text) {
        auto r = parseHostRule(text, error);
        REQUIRE_MESSAGE(r, text, ": ", error);
        return *r;
    };
    HostRule plain = rule("api.weather.gov");
    CHECK(plain.allows("api.weather.gov", 443, true));
    CHECK(!plain.allows("api.weather.gov", 80, false));     // HTTPS only
    CHECK(!plain.allows("api.weather.gov", 8443, true));    // 443 only
    CHECK(!plain.allows("x.api.weather.gov", 443, true));
    CHECK(!plain.allows("weather.gov", 443, true));

    HostRule sub = rule("*.stripe.com");
    CHECK(sub.allows("api.stripe.com", 443, true));
    CHECK(sub.allows("a.b.stripe.com", 443, true));
    CHECK(!sub.allows("stripe.com", 443, true));
    CHECK(!sub.allows("evilstripe.com", 443, true));

    HostRule http = rule("http://feeds.example.org");
    CHECK(http.allows("feeds.example.org", 80, false));
    CHECK(http.allows("feeds.example.org", 443, true));

    HostRule port = rule("api.example.com:8443");
    CHECK(port.allows("api.example.com", 8443, true));
    CHECK(!port.allows("api.example.com", 443, true));

    HostRule any = rule("*");
    CHECK(any.allows("anything.example", 443, true));
    CHECK(!any.allows("anything.example", 80, false));

    for (const char* bad : {"https://api.example.com/v1", "*.com", "api.*.com", "a*b.com", "user@host.com",
                            "host:0", "host:http", "bücher.de", ""}) {
        CHECK_MESSAGE(!parseHostRule(bad, error), bad);
    }

    OutboundPolicy p;
    for (const char* m : {"owner@example.com", "*@staff.example.com"}) p.mailTo.push_back(*parseMailRule(m, error));
    CHECK(p.allowsMailTo("owner@example.com"));
    CHECK(p.allowsMailTo("ann@staff.example.com"));
    CHECK(!p.allowsMailTo("ann@example.com"));
    CHECK(!p.allowsMailTo("owner@example.com.evil.com"));
    CHECK(!parseMailRule("not an address", error));
    CHECK(!parseMailRule("a@b", error));
}

TEST_CASE("HMAC-SHA-256 (RFC 4231) and verifying signatures") {
    CHECK(hex(hmacSha256(std::string(20, '\x0b'), "Hi There")) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(hex(hmacSha256("Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(hex(hmacSha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    SecretMap secrets;
    secrets["hook"] = Secret{"whsec", {}, true};
    secrets["api"] = Secret{"key", {"api.test"}, false};
    secrets["db"] = Secret{"pw", {}, false};
    OutboundClient client(OutboundSettings{}, secrets);
    std::string body = "{\"id\":1}";
    std::string mac = hmacSha256("whsec", body);
    CHECK(client.verifyHmac("sha256", "hook", body, toHex(mac)).value());
    CHECK(client.verifyHmac("SHA256", "hook", body, "sha256=" + toHex(mac)).value());   // GitHub's form
    std::string upper = toHex(mac);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(client.verifyHmac("sha256", "hook", body, upper).value());
    std::string b64 = toBase64Url(mac);
    CHECK(client.verifyHmac("sha256", "hook", body, b64).value());
    CHECK(!client.verifyHmac("sha256", "hook", body + " ", toHex(mac)).value());
    CHECK(!client.verifyHmac("sha256", "hook", body, "").value());
    CHECK(!client.verifyHmac("sha256", "hook", body, "zz").value());
    CHECK(client.verifyHmac("sha256", "api", "x", toHex(hmacSha256("key", "x"))).value());   // a sending secret can verify
    CHECK(!client.verifyHmac("sha256", "db", body, toHex(mac)));                           // engine-only: refused
    CHECK(!client.verifyHmac("sha256", "nope", body, toHex(mac)));
    CHECK(!client.verifyHmac("md5", "hook", body, toHex(mac)));

    auto uses = client.secretUses();
    CHECK(uses["hook"] == "verify");
    CHECK(uses["api"] == "send");
    CHECK(uses["db"] == "engine");
}

TEST_CASE("a request to an allowed host, with a secret in a header and the URL") {
    FakeHttp server([](const FakeHttp::Request&) {
        return FakeHttp::response(200, "{\"temp\": 21}", "Content-Type: application/json\r\nX-Two: a\r\nX-Two: b\r\n");
    });
    SecretMap secrets;
    secrets["weatherKey"] = Secret{"s3cr3t value&x=1", {"api.test"}, false};
    Calls calls;
    OutboundClient client(settingsFor(server.port()), secrets, calls.hook());
    std::string base = "http://api.test:" + std::to_string(server.port());
    OutboundPolicy policy = policyFor({"http://api.test:" + std::to_string(server.port())});

    OutboundRequest r;
    r.url = {base + "/points?key=", SecretRef{"weatherKey"}, "&city=San Jose"};
    r.headers.push_back({"Authorization", {"Bearer ", SecretRef{"weatherKey"}}});
    r.headers.push_back({"Accept", {"application/json"}});
    auto resp = client.send(policy, r, kTime);
    REQUIRE_MESSAGE(static_cast<bool>(resp), (resp ? std::string() : resp.error().message));
    CHECK(resp.value().status == 200);
    CHECK(resp.value().body == "{\"temp\": 21}");
    bool joined = false;
    for (const auto& [name, value] : resp.value().headers) joined = joined || (name == "x-two" && value == "a, b");
    CHECK(joined);

    REQUIRE(server.count() == 1);
    FakeHttp::Request got = server.requests()[0];
    // The value went out, percent-encoded in the URL so it can't add a parameter.
    CHECK(got.target() == "/points?key=s3cr3t%20value%26x%3D1&city=San%20Jose");
    CHECK(contains(got.head, "Authorization: Bearer s3cr3t value&x=1\r\n"));
    CHECK(contains(got.head, "User-Agent: Fourfold/"));
    // The log has neither the value nor the query.
    std::string log = calls.text();
    CHECK(!contains(log, "s3cr3t"));
    CHECK(contains(log, "/points?…"));
}

TEST_CASE("SSRF: private and reserved addresses are refused, however they're reached") {
    std::atomic<int> hits{0};
    FakeHttp server([&](const FakeHttp::Request&) {
        hits++;
        return FakeHttp::response(200, "secret internal data");
    });
    std::string port = std::to_string(server.port());
    Calls calls;
    // No allowAddresses: loopback is refused like any private address.
    OutboundSettings s = settingsFor(server.port(), {"meta.test:" + port + ":169.254.169.254",
                                                     "private.test:" + port + ":10.0.0.1",
                                                     "mapped.test:" + port + ":::ffff:127.0.0.1",
                                                     "mixed.test:" + port + ":10.0.0.1,169.254.169.254"},
                                     false);
    s.connectTimeout = std::chrono::milliseconds(500);
    OutboundClient client(s, {}, calls.hook());
    OutboundPolicy any = policyFor({"http://*:" + port});

    struct Case {
        std::string url;
        std::string why;
    };
    for (const Case& c : std::vector<Case>{
             {"http://api.test:" + port + "/", "loopback"},           // a name for 127.0.0.1
             {"http://localhost:" + port + "/", "loopback"},          // localhost by name
             {"http://127.0.0.1:" + port + "/", "loopback"},          // ...and by number
             {"http://[::1]:" + port + "/", "loopback"},
             {"http://[::ffff:127.0.0.1]:" + port + "/", "loopback"},  // IPv4-mapped IPv6
             {"http://mapped.test:" + port + "/", "loopback"},        // a name for it
             {"http://meta.test:" + port + "/", "metadata"},          // cloud metadata
             {"http://169.254.169.254:" + port + "/latest/meta-data/", "metadata"},
             {"http://private.test:" + port + "/", "private"},
             {"http://mixed.test:" + port + "/", "refused"},          // every address private
             {"http://0.0.0.0:" + port + "/", "this network"},
         }) {
        auto r = client.send(any, get(c.url), kTime);
        CHECK_MESSAGE(!r, c.url);
        if (!r) CHECK_MESSAGE(contains(r.error().message, c.why), c.url, ": ", r.error().message);
    }
    CHECK(hits == 0);
}

TEST_CASE("DNS rebinding: the address is checked as it's connected to") {
    // The same name, resolving first to an allowed address and then to a
    // private one: each request checks the address it actually connects to,
    // so there's no gap between checking and connecting to exploit.
    std::atomic<int> hits{0};
    FakeHttp server([&](const FakeHttp::Request&) {
        hits++;
        return FakeHttp::response(200, "ok");
    });
    std::string port = std::to_string(server.port());
    OutboundPolicy policy = policyFor({"http://rebind.test:" + port});
    OutboundSettings first = settingsFor(server.port(), {"rebind.test:" + port + ":127.0.0.1"});
    OutboundClient before(first, {});
    REQUIRE(before.send(policy, get("http://rebind.test:" + port + "/"), kTime));
    CHECK(hits == 1);

    OutboundSettings second = settingsFor(server.port(), {"rebind.test:" + port + ":169.254.169.254"});
    second.connectTimeout = std::chrono::milliseconds(500);
    OutboundClient after(second, {});
    auto r = after.send(policy, get("http://rebind.test:" + port + "/"), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "169.254.169.254"));
    CHECK(hits == 1);

    // A name with a public and a private address is only ever connected to
    // at the one it may reach (here, the allowed loopback one).
    OutboundSettings both = settingsFor(server.port(), {"both.test:" + port + ":169.254.169.254,127.0.0.1"});
    OutboundClient mixed(both, {});
    auto ok = mixed.send(policyFor({"http://both.test:" + port}), get("http://both.test:" + port + "/"), kTime);
    CHECK_MESSAGE(static_cast<bool>(ok), (ok ? std::string() : ok.error().message));
    CHECK(hits == 2);
}

TEST_CASE("the allowlist") {
    std::atomic<int> hits{0};
    FakeHttp server([&](const FakeHttp::Request&) {
        hits++;
        return FakeHttp::response(200, "ok");
    });
    std::string port = std::to_string(server.port());
    OutboundClient client(settingsFor(server.port()), {});
    std::string url = "http://api.test:" + port + "/";

    // No outbound.ms: nothing.
    auto r = client.send(OutboundPolicy{}, get(url), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "no code/outbound.ms"));
    // Another host; HTTPS only; another port.
    CHECK(!client.send(policyFor({"other.test"}), get(url), kTime));
    r = client.send(policyFor({"api.test:" + port}), get(url), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "plain HTTP"));
    CHECK(!client.send(policyFor({"http://api.test"}), get(url), kTime));
    CHECK(hits == 0);
    CHECK(client.send(policyFor({"http://api.test:" + port}), get(url), kTime));
    CHECK(hits == 1);

    // The config can only tighten: a cap the app's list must fit within,
    // or outbound turned off.
    OutboundSettings capped = settingsFor(server.port());
    std::string error;
    capped.cap = std::vector<HostRule>{*parseHostRule("other.test", error)};
    OutboundClient cappedClient(capped, {});
    r = cappedClient.send(policyFor({"http://api.test:" + port}), get(url), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "config"));
    OutboundSettings off = settingsFor(server.port());
    off.enabled = false;
    OutboundClient offClient(off, {});
    CHECK(!offClient.send(policyFor({"http://api.test:" + port}), get(url), kTime));
    CHECK(hits == 1);
}

TEST_CASE("secrets go only to their own hosts") {
    std::atomic<int> hits{0};
    FakeHttp server([&](const FakeHttp::Request&) {
        hits++;
        return FakeHttp::response(200, "ok");
    });
    std::string port = std::to_string(server.port());
    SecretMap secrets;
    secrets["key"] = Secret{"TOPSECRET", {"api.test"}, false};
    secrets["star"] = Secret{"STARSECRET", {"*.test"}, false};
    secrets["hook"] = Secret{"HOOKSECRET", {}, true};
    secrets["db"] = Secret{"DBSECRET", {}, false};
    Calls calls;
    OutboundClient client(settingsFor(server.port()), secrets, calls.hook());
    OutboundPolicy policy = policyFor({"http://api.test:" + port, "http://other.test:" + port});

    auto withHeader = [&](const std::string& url, const std::string& secret) {
        OutboundRequest r = get(url);
        r.headers.push_back({"X-Key", {SecretRef{secret}}});
        return client.send(policy, r, kTime);
    };
    // The wrong host, a verify-only secret, an engine-only one: refused, unsent.
    auto r = withHeader("http://other.test:" + port + "/", "key");
    REQUIRE(!r);
    CHECK(contains(r.error().message, "only to its own hosts"));
    CHECK(!withHeader("http://api.test:" + port + "/", "hook"));
    r = withHeader("http://api.test:" + port + "/", "db");
    REQUIRE(!r);
    CHECK(contains(r.error().message, "engine only"));
    CHECK(!withHeader("http://api.test:" + port + "/", "missing"));
    CHECK(hits == 0);
    // A "*." host covers subdomains.
    CHECK(withHeader("http://other.test:" + port + "/", "star"));
    CHECK(hits == 1);

    // Never in the host part of a URL.
    OutboundRequest inHost;
    inHost.url = {"http://", SecretRef{"key"}, ".api.test:" + port + "/"};
    r = client.send(policy, inHost, kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "path or query"));
    // No line breaks smuggled into a header.
    OutboundRequest crlf = get("http://api.test:" + port + "/");
    crlf.headers.push_back({"X-A", {"a\r\nX-Injected: 1"}});
    CHECK(!client.send(policy, crlf, kTime));
    OutboundRequest host = get("http://api.test:" + port + "/");
    host.headers.push_back({"Host", {"evil.test"}});
    CHECK(!client.send(policy, host, kTime));
    CHECK(hits == 1);

    CHECK(!contains(calls.text(), "SECRET"));
}

TEST_CASE("redirects are followed, and checked at every hop") {
    std::string port;
    std::atomic<int> hits{0};
    FakeHttp server([&](const FakeHttp::Request& r) {
        hits++;
        std::string t = r.target();
        auto to = [&](const std::string& where, int status = 302) {
            return FakeHttp::response(status, "", "Location: " + where + "\r\n");
        };
        if (t == "/to-meta") return to("http://meta.test:" + port + "/");
        if (t == "/to-meta-ip") return to("http://169.254.169.254/latest/meta-data/");
        if (t == "/to-unlisted") return to("http://unlisted.test:" + port + "/");
        if (t == "/to-other") return to("http://other.test:" + port + "/landed");
        if (t == "/relative") return to("/landed");
        if (t == "/see-other") return to("/landed", 303);
        if (t == "/keep") return to("/landed", 307);
        if (t == "/loop") return to("/loop");
        return FakeHttp::response(200, r.method() + " " + t + " " + r.body);
    });
    port = std::to_string(server.port());
    SecretMap secrets;
    secrets["key"] = Secret{"TOPSECRET", {"api.test"}, false};
    OutboundSettings s = settingsFor(server.port(), {"meta.test:" + port + ":169.254.169.254",
                                                     "unlisted.test:" + port + ":127.0.0.1"});
    s.connectTimeout = std::chrono::milliseconds(500);
    OutboundClient client(s, secrets);
    OutboundPolicy policy =
        policyFor({"http://api.test:" + port, "http://other.test:" + port, "http://meta.test:" + port, "http://*"});
    std::string base = "http://api.test:" + port;

    auto r = client.send(policy, get(base + "/to-meta"), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "169.254.169.254"));
    r = client.send(policy, get(base + "/to-meta-ip"), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "metadata"));
    r = client.send(policy, get(base + "/to-unlisted"), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "a redirect to"));

    r = client.send(policy, get(base + "/relative"), kTime);
    REQUIRE(r);
    CHECK(r.value().body == "GET /landed ");

    // POST then 303: a GET, without the body. 307 keeps both.
    OutboundRequest post = get(base + "/see-other");
    post.method = "POST";
    post.body = "data";
    r = client.send(policy, post, kTime);
    REQUIRE(r);
    CHECK(r.value().body == "GET /landed ");
    post.url = {base + "/keep"};
    r = client.send(policy, post, kTime);
    REQUIRE(r);
    CHECK(r.value().body == "POST /landed data");

    r = client.send(policy, get(base + "/loop"), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "redirects"));

    // A secret doesn't follow a redirect to another host; a plain
    // Authorization header is dropped there.
    OutboundRequest withSecret = get(base + "/to-other");
    withSecret.headers.push_back({"X-Key", {SecretRef{"key"}}});
    int before = hits;
    r = client.send(policy, withSecret, kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "only to its own hosts"));
    CHECK(hits == before + 1);   // the first hop only
    OutboundRequest withAuth = get(base + "/to-other");
    withAuth.headers.push_back({"Authorization", {"Basic abc"}});
    r = client.send(policy, withAuth, kTime);
    REQUIRE(r);
    CHECK(!contains(server.requests().back().head, "Authorization"));
}

TEST_CASE("limits: size (also decompressed), time and rate") {
    std::string big(64 * 1024, 'x');
    // 4 MB of zeros, deflated to a few KB: a decompression bomb.
    std::string zeros(4 * 1024 * 1024, '\0');
    uLongf packedSize = compressBound(zeros.size());
    std::string packed(packedSize, '\0');
    REQUIRE(compress2(reinterpret_cast<Bytef*>(&packed[0]), &packedSize,
                      reinterpret_cast<const Bytef*>(zeros.data()), zeros.size(), 9) == Z_OK);
    packed.resize(packedSize);
    FakeHttp server([&](const FakeHttp::Request& r) {
        if (r.target() == "/big") return FakeHttp::response(200, big);
        if (r.target() == "/bomb") return FakeHttp::response(200, packed, "Content-Encoding: deflate\r\n");
        if (r.target() == "/small-deflated") {
            std::string text = "hello hello hello";
            uLongf n = compressBound(text.size());
            std::string out(n, '\0');
            compress2(reinterpret_cast<Bytef*>(&out[0]), &n, reinterpret_cast<const Bytef*>(text.data()), text.size(), 9);
            out.resize(n);
            return FakeHttp::response(200, out, "Content-Encoding: deflate\r\n");
        }
        if (r.target() == "/slow") std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        return FakeHttp::response(200, "ok");
    });
    std::string base = "http://api.test:" + std::to_string(server.port());
    OutboundPolicy policy = policyFor({base});
    OutboundSettings s = settingsFor(server.port());
    s.maxResponseBytes = 32 * 1024;
    s.timeout = std::chrono::milliseconds(500);
    s.perMinute = 6;
    OutboundClient client(s, {});

    auto r = client.send(policy, get(base + "/big"), kTime);
    REQUIRE(!r);
    CHECK(r.error().code == ErrorCode::TooLarge);
    CHECK(packed.size() < s.maxResponseBytes);
    r = client.send(policy, get(base + "/bomb"), kTime);
    REQUIRE(!r);
    CHECK(r.error().code == ErrorCode::TooLarge);
    r = client.send(policy, get(base + "/small-deflated"), kTime);
    REQUIRE(r);
    CHECK(r.value().body == "hello hello hello");

    auto started = std::chrono::steady_clock::now();
    r = client.send(policy, get(base + "/slow"), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "too long"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1400));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // the fake server answers one at a time
    // The run's own time left is a limit too.
    r = client.send(policy, get(base + "/"), std::chrono::milliseconds(0));
    CHECK(!r);

    // Six a minute: four so far, then two more, then refused.
    CHECK(client.send(policy, get(base + "/"), kTime));
    CHECK(client.send(policy, get(base + "/"), kTime));
    r = client.send(policy, get(base + "/"), kTime);
    REQUIRE(!r);
    CHECK(contains(r.error().message, "a minute"));
}

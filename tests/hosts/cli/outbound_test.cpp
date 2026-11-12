// End to end (M9): outbound HTTP with secrets, webhooks and app mail, through
// `fourfold dev`'s server on a real socket, with a fake API server.
//
// The exit criteria (milestones.md, M9) tested here: a secret can't be made
// to appear in a page, a log, a data file, the session, a mail, or a
// request to the wrong host; a webhook with a good signature is accepted,
// and one with a bad or missing one refused, without a CSRF token; app mail
// goes to a user by id and to an allowed address, and is refused to
// others; the weather sample's pattern works against a local server. (The
// SSRF and limit tests are in tests/outbound.)
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "app_server.h"
#include "base/crypto.h"
#include "browser.h"
#include "doctest.h"
#include "fake_http.h"
#include "outbound/outbound_client.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::test::bodyOf;
using fourfold::test::Browser;
using fourfold::test::FakeHttp;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int statusOf(const std::string& response) {
    return response.size() > 12 ? std::atoi(response.c_str() + 9) : 0;
}

struct Server {
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;

    explicit Server(const std::string& root) {
        server = AppServer::startDev(root, 0, out, err);
        REQUIRE_MESSAGE(server, err.str());
        REQUIRE_MESSAGE(server->compiled(), (err.str() + out.str()));
        serving = std::thread([this] { server->loop().run(); });
    }
    ~Server() {
        server->loop().stop();
        serving.join();
    }
    uint16_t port() const { return server->port(); }
};

// Every file under `dir`, read whole, joined.
std::string everything(const std::filesystem::path& dir) {
    std::string all;
    if (!std::filesystem::exists(dir)) return all;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::ifstream in(e.path(), std::ios::binary);
        all += std::string(std::istreambuf_iterator<char>(in), {});
    }
    return all;
}

const char* const kSecretValue = "Sup3rS3cretKey";
const char* const kHookValue = "whsec_t0ps3cret";

std::string makeApp(const TempDir& dir, int apiPort) {
    std::string api = "http://127.0.0.1:" + std::to_string(apiPort);
    std::string other = "http://localhost:" + std::to_string(apiPort);
    dir.write("app/private/config/app.ms",
              "config = {}\n"
              "config.outbound = {\"allowAddresses\": [\"127.0.0.1\", \"::1\"]}\n");
    dir.write("app/private/config/secrets.ms",
              std::string("secrets = {}\n") +
                  "secrets.weatherKey = {\"value\": \"" + kSecretValue + "\", \"hosts\": [\"127.0.0.1\"]}\n" +
                  "secrets.hookKey = {\"value\": \"" + kHookValue + "\", \"verify\": true}\n" +
                  "secrets.smtpPassword = {\"value\": \"engineonly\"}\n");
    dir.write("app/code/outbound.ms", "allow = [\"" + api + "\", \"" + other + "\"]\n"
                                      "mailTo = [\"owner@example.com\"]\n");
    dir.write("app/code/mail/welcome.md", "# Welcome, {{name}}!\n\nThanks for joining.\n");
    dir.write("app/code/frags/show.html", "<p>{{value}}</p>\n");
    dir.write("app/code/pages/api.ms",
              "get.weather = function(req)\n"
              "    resp = http.get(\"" + api + "/forecast?city=\" + req.query.city, "
              "{\"Authorization\": [\"Bearer \", secrets.weatherKey]})\n"
              "    if resp isa error then return status(502, str(resp))\n"
              "    return json({\"status\": resp.status, \"temp\": resp.json.temp, \"type\": resp.headers[\"content-type\"]})\n"
              "end function\n"
              "get.post = function(req)\n"
              "    resp = http.post(\"" + api + "/echo\", {\"a\": [1, 2]})\n"
              "    return resp.body\n"
              "end function\n"
              "get.missing = function(req)\n"
              "    resp = http.get(\"" + api + "/missing\")\n"
              "    return json({\"status\": resp.status, \"body\": resp.body})\n"
              "end function\n"
              "get.unlisted = function(req)\n"
              "    return str(http.get(\"https://example.com/\"))\n"
              "end function\n"
              "get.wrongHost = function(req)\n"
              "    return str(http.get(\"" + other + "/x\", {\"X-Key\": secrets.weatherKey}))\n"
              "end function\n"
              "get.engineOnly = function(req)\n"
              "    return str(http.get(\"" + api + "/x\", {\"X-Key\": secrets.smtpPassword}))\n"
              "end function\n"
              // Every way a script might try to get the value out.
              "get.leakText = function(req)\n"
              "    return \"[\" + str(secrets.weatherKey) + \"]\"\n"
              "end function\n"
              "get.leakPage = function(req)\n"
              "    return frag(\"show\", {\"value\": secrets.weatherKey})\n"
              "end function\n"
              "get.leakJson = function(req)\n"
              "    return json({\"k\": secrets.weatherKey})\n"
              "end function\n"
              "get.leakData = function(req)\n"
              "    return str(data.set(\"k\", secrets.weatherKey))\n"
              "end function\n"
              "get.leakSession = function(req)\n"
              "    session.k = secrets.weatherKey\n"
              "    return \"saved?\"\n"
              "end function\n"
              "get.leakPrint = function(req)\n"
              "    print secrets.weatherKey\n"
              "    print secrets\n"
              "    return \"printed\"\n"
              "end function\n"
              "get.leakMail = function(req)\n"
              "    return str(mail.send(\"owner@example.com\", \"Key\", \"welcome\", {\"name\": secrets.weatherKey}))\n"
              "end function\n"
              "get.leakError = function(req)\n"
              "    x = secrets.weatherKey + 1\n"
              "    return str(x)\n"
              "end function\n"
              // Webhooks.
              "webhook.hook = function(req)\n"
              "    sig = \"\"\n"
              "    if req.headers.hasIndex(\"x-signature\") then sig = req.headers[\"x-signature\"]\n"
              "    if not hmac.verify(\"sha256\", secrets.hookKey, req.rawBody, sig) then return status(400, \"bad signature\")\n"
              "    return json({\"event\": req.json.event, \"raw\": req.rawBody})\n"
              "end function\n"
              "post.form = function(req)\n"
              "    return \"posted\"\n"
              "end function\n"
              "get.noRawBody = function(req)\n"
              "    return str(req.hasIndex(\"rawBody\"))\n"
              "end function\n"
              // Mail.
              "get.mailUser = function(req)\n"
              "    return str(mail.send(req.query.id, \"Hello there\", \"welcome\", {\"name\": \"Ann\"}))\n"
              "end function\n"
              "get.mailOwner = function(req)\n"
              "    return str(mail.send(\"Owner@Example.com\", \"A note\", \"welcome\", {\"name\": \"Owner\"}))\n"
              "end function\n"
              "get.mailStranger = function(req)\n"
              "    return str(mail.send(\"stranger@example.com\", \"Spam\", \"welcome\", {\"name\": \"X\"}))\n"
              "end function\n"
              "get.mailBadSubject = function(req)\n"
              "    return str(mail.send(\"owner@example.com\", \"Hi\" + char(10) + \"Bcc: all@example.com\", "
              "\"welcome\", {\"name\": \"X\"}))\n"
              "end function\n"
              "get.mailBcc = function(req)\n"
              "    return str(mail.send(req.query.id, \"Your order\", \"welcome\", {\"name\": \"Ann\"}, "
              "{\"bcc\": [\"owner@example.com\", req.query.id]}))\n"
              "end function\n"
              "get.mailBccStranger = function(req)\n"
              "    return str(mail.send(req.query.id, \"Hi\", \"welcome\", {\"name\": \"X\"}, "
              "{\"bcc\": \"stranger@example.com\"}))\n"
              "end function\n"
              "get.mailCc = function(req)\n"
              "    return str(mail.send(req.query.id, \"Hi\", \"welcome\", {\"name\": \"X\"}, "
              "{\"cc\": \"owner@example.com\"}))\n"
              "end function\n"
              "get.mailNoUser = function(req)\n"
              "    return str(mail.send(\"nobody-by-this-id\", \"Hi\", \"welcome\", {\"name\": \"X\"}))\n"
              "end function\n");
    return dir.at("app");
}

}  // namespace

TEST_CASE("outbound HTTP from app code, with secrets that can't leak") {
    FakeHttp api([](const FakeHttp::Request& r) {
        if (r.target().rfind("/forecast", 0) == 0) {
            return FakeHttp::response(200, "{\"temp\": 21.5}", "Content-Type: application/json\r\n");
        }
        if (r.target() == "/echo") return FakeHttp::response(200, r.body, "Content-Type: text/plain\r\n");
        return FakeHttp::response(404, "nope");
    });
    TempDir dir;
    std::string root = makeApp(dir, api.port());
    Server s(root);
    Browser b(s.port());

    std::string r = b.get("/api/weather?city=San%20Jose");
    REQUIRE_MESSAGE(statusOf(r) == 200, r);
    CHECK(contains(bodyOf(r), "\"temp\":21.5"));
    CHECK(contains(bodyOf(r), "application/json"));
    REQUIRE(api.count() == 1);
    CHECK(contains(api.requests()[0].head, std::string("Authorization: Bearer ") + kSecretValue));
    CHECK(api.requests()[0].target() == "/forecast?city=San%20Jose");

    r = b.get("/api/post");
    CHECK(bodyOf(r) == "{\"a\":[1,2]}");
    CHECK(contains(api.requests().back().head, "Content-Type: application/json"));
    r = b.get("/api/missing");
    CHECK(contains(bodyOf(r), "\"status\":404"));
    CHECK(contains(bodyOf(r), "\"body\":\"nope\""));

    size_t before = api.count();
    // A failed request is an error value; returned, it's a 500, and dev
    // mode's error page says why.
    r = b.get("/api/unlisted");
    CHECK(statusOf(r) == 500);
    CHECK(contains(bodyOf(r), "list of hosts"));
    r = b.get("/api/wrongHost");
    CHECK(contains(bodyOf(r), "only to its own hosts"));
    r = b.get("/api/engineOnly");
    CHECK(contains(bodyOf(r), "engine only"));
    CHECK(api.count() == before);   // none of them sent

    // Nowhere for the value to come out.
    std::vector<std::string> responses;
    for (const char* path : {"/api/leakText", "/api/leakPage", "/api/leakJson", "/api/leakData", "/api/leakSession",
                             "/api/leakPrint", "/api/leakMail", "/api/leakError"}) {
        responses.push_back(b.get(path));
        CHECK_MESSAGE(!contains(responses.back(), kSecretValue), path, ": ", responses.back());
    }
    CHECK(statusOf(responses[0]) == 200);    // str() gives only a placeholder
    CHECK(statusOf(responses[1]) == 500);   // a template refuses it
    CHECK(statusOf(responses[2]) == 500);   // json() refuses it
    CHECK(contains(bodyOf(responses[3]), "secret"));   // data.set refuses it
    CHECK(statusOf(responses[4]) == 500);   // the session can't hold it
    CHECK(statusOf(responses[6]) == 500);   // the mail template refuses it
    CHECK(contains(bodyOf(responses[6]), "secret"));
    CHECK(!contains(s.out.str(), kSecretValue));
    CHECK(!contains(s.err.str(), kSecretValue));
    CHECK(!contains(everything(dir.at("app/private/state")), kSecretValue));
    CHECK(!contains(everything(dir.at("app/data")), kSecretValue));

    // The outbound report: hosts, mailTo, secrets (names and hosts only),
    // webhooks, and the latest requests, refusals included.
    std::string report = bodyOf(b.get("/__dev/outbound"));
    CHECK(contains(report, "http://127.0.0.1:" + std::to_string(api.port())));
    CHECK(contains(report, "owner@example.com"));
    CHECK(contains(report, "weatherKey"));
    CHECK(contains(report, "hookKey"));
    CHECK(contains(report, "engine only"));
    CHECK(contains(report, "POST /api/hook"));
    CHECK(contains(report, "/forecast?…"));
    CHECK(contains(report, "refused"));
    CHECK(!contains(report, kSecretValue));
    CHECK(!contains(report, kHookValue));
    CHECK(!contains(report, "engineonly"));
    std::string json = bodyOf(b.get("/__dev/outbound.json"));
    CHECK(contains(json, "\"webhooks\""));
    CHECK(!contains(json, kSecretValue));
    // The engine log has each request, without the secret.
    std::string log = everything(dir.at("app/private/state/logs"));
    CHECK(contains(log, "outbound"));
    CHECK(contains(log, "/forecast?…"));
}

TEST_CASE("webhooks: no CSRF token, a signature instead") {
    FakeHttp api([](const FakeHttp::Request&) { return FakeHttp::response(200, "{}"); });
    TempDir dir;
    Server s(makeApp(dir, api.port()));
    Browser b(s.port());   // no cookies, no token: like Stripe or GitHub calling in

    std::string body = "{\"event\":\"paid\"}";
    std::string sig = toHex(hmacSha256(kHookValue, body));
    std::string r = b.send("POST", "/api/hook", body, false, "X-Signature: sha256=" + sig + "\r\n", "application/json");
    REQUIRE_MESSAGE(statusOf(r) == 200, r);
    CHECK(contains(bodyOf(r), "\"event\":\"paid\""));
    CHECK(contains(bodyOf(r), "\\\"event\\\""));   // rawBody, as sent

    r = b.send("POST", "/api/hook", body, false, "X-Signature: " + std::string(64, '0') + "\r\n", "application/json");
    CHECK(statusOf(r) == 400);
    r = b.send("POST", "/api/hook", body, false, "", "application/json");
    CHECK(statusOf(r) == 400);
    r = b.send("POST", "/api/hook", body + " ", false, "X-Signature: " + sig + "\r\n", "application/json");
    CHECK(statusOf(r) == 400);
    // Even from another origin: a webhook's caller isn't a browser.
    r = b.send("POST", "/api/hook", body, false, "Origin: https://evil.example\r\nX-Signature: " + sig + "\r\n",
               "application/json");
    CHECK(statusOf(r) == 200);
    // A webhook answers POST only.
    CHECK(statusOf(b.get("/api/hook")) == 405);

    // An ordinary post handler still needs its token.
    r = b.send("POST", "/api/form", "x=1");
    CHECK(statusOf(r) == 403);
    // Only a webhook gets req.rawBody.
    CHECK(bodyOf(b.get("/api/noRawBody")) == "0");
}

TEST_CASE("app mail: to users by id, and to allowed addresses only") {
    FakeHttp api([](const FakeHttp::Request&) { return FakeHttp::response(200, "{}"); });
    TempDir dir;
    Server s(makeApp(dir, api.port()));
    Browser b(s.port());
    auto user = s.server->users().create("ann@example.com", "Ann");
    REQUIRE(user);

    std::string r = b.get("/api/mailUser?id=" + user.value().id);
    REQUIRE_MESSAGE(statusOf(r) == 200, r);
    CHECK(bodyOf(r) == "");   // str(null)
    std::string mail = s.out.str();
    CHECK(contains(mail, "ann@example.com"));
    CHECK(contains(mail, "Hello there"));
    CHECK(contains(mail, "Welcome, Ann!"));

    r = b.get("/api/mailOwner");
    CHECK(statusOf(r) == 200);
    CHECK(contains(s.out.str(), "owner@example.com"));

    r = b.get("/api/mailStranger");
    CHECK(statusOf(r) == 500);
    CHECK(contains(bodyOf(r), "mailTo"));
    CHECK(!contains(s.out.str(), "stranger@example.com"));
    r = b.get("/api/mailBadSubject");
    CHECK(contains(bodyOf(r), "line breaks"));
    CHECK(!contains(s.out.str(), "all@example.com"));
    r = b.get("/api/mailNoUser");
    CHECK(contains(bodyOf(r), "no user"));

    // A blind copy to the owner (and a repeat of the recipient, dropped).
    size_t before = s.out.str().size();
    r = b.get("/api/mailBcc?id=" + user.value().id);
    REQUIRE_MESSAGE(statusOf(r) == 200, r);
    std::string sent = s.out.str().substr(before);
    CHECK(contains(sent, "To: ann@example.com"));
    CHECK(contains(sent, "Bcc (not in the message's headers): owner@example.com\n"));
    // A blind copy goes by the same rules as `to`, and there's no cc.
    before = s.out.str().size();
    r = b.get("/api/mailBccStranger?id=" + user.value().id);
    CHECK(statusOf(r) == 500);
    CHECK(contains(bodyOf(r), "mailTo"));
    r = b.get("/api/mailCc?id=" + user.value().id);
    CHECK(statusOf(r) == 500);
    CHECK(contains(bodyOf(r), "no cc"));
    CHECK(!contains(s.out.str().substr(before), "Mail (not sent"));   // nothing sent (only the errors printed)
}

TEST_CASE("app mail: each recipient counts against the hourly limit") {
    FakeHttp api([](const FakeHttp::Request&) { return FakeHttp::response(200, "{}"); });
    TempDir dir;
    std::string root = makeApp(dir, api.port());
    dir.write("app/private/config/app.ms", "config = {}\nconfig.mail = {\"appPerHour\": 3}\n");
    Server s(root);
    Browser b(s.port());
    auto user = s.server->users().create("ann@example.com", "Ann");
    REQUIRE(user);
    CHECK(statusOf(b.get("/api/mailBcc?id=" + user.value().id)) == 200);   // 2: Ann and the owner
    std::string r = b.get("/api/mailBcc?id=" + user.value().id);             // 4 would be over 3
    CHECK(statusOf(r) == 500);
    CHECK(contains(bodyOf(r), "each recipient counts"));
    CHECK(statusOf(b.get("/api/mailOwner")) == 200);                          // exactly 3
}

TEST_CASE("the weather sample, against a local stand-in for api.weather.gov") {
    namespace fs = std::filesystem;
    std::string base;
    FakeHttp api([&](const FakeHttp::Request& r) {
        std::string json = "Content-Type: application/geo+json\r\n";
        if (r.target() == "/points/39.7456,-97.0892") {
            return FakeHttp::response(200, "{\"properties\": {\"forecast\": \"" + base + "/gridpoints/TOP/32,81/forecast\"}}",
                                      json);
        }
        if (r.target() == "/points/1,2") {
            // A response that tries to send the app to cloud metadata.
            return FakeHttp::response(200, "{\"properties\": {\"forecast\": \"http://169.254.169.254/latest/\"}}", json);
        }
        if (r.target() == "/gridpoints/TOP/32,81/forecast") {
            return FakeHttp::response(
                200,
                "{\"properties\": {\"periods\": ["
                "{\"name\": \"Tonight\", \"temperature\": 41, \"temperatureUnit\": \"F\", \"shortForecast\": \"Clear <b>\"},"
                "{\"name\": \"Thursday\", \"temperature\": 63, \"temperatureUnit\": \"F\", \"shortForecast\": \"Sunny\"}]}}",
                json);
        }
        return FakeHttp::response(404, "{}", json);
    });
    base = "http://127.0.0.1:" + std::to_string(api.port());

    TempDir dir;
    fs::path from = fs::path(FOURFOLD_TESTS_DIR) / ".." / "examples" / "weather";
    fs::path to = dir.at("weather");
    fs::create_directories(to);
    fs::copy(from / "code", to / "code", fs::copy_options::recursive);
    fs::create_directories(to / "private" / "config");
    fs::copy(from / "private" / "config" / "secrets.ms", to / "private" / "config" / "secrets.ms");
    dir.write("weather/private/config/app.ms", "config = {}\nconfig.outbound = {\"allowAddresses\": [\"127.0.0.1\"]}\n");
    dir.write("weather/code/outbound.ms", "allow = [\"" + base + "\"]\nmailTo = [\"owner@example.com\"]\n");
    std::ifstream in(to / "code" / "pages" / "index.ms");
    std::string index((std::istreambuf_iterator<char>(in)), {});
    size_t at = index.find("https://api.weather.gov");
    REQUIRE(at != std::string::npos);
    index.replace(at, std::strlen("https://api.weather.gov"), base);
    dir.write("weather/code/pages/index.ms", index);

    Server s(to.string());
    Browser b(s.port());
    std::string r = b.get("/");
    REQUIRE_MESSAGE(statusOf(r) == 200, r);
    CHECK(contains(r, "Tonight"));
    CHECK(contains(r, "41°F"));
    CHECK(contains(r, "Clear &lt;b&gt;"));   // a response is untrusted text
    r = b.get("/?lat=1&lon=2");
    CHECK(statusOf(r) == 200);
    CHECK(contains(r, "169.254.169.254"));   // the problem, shown
    CHECK(contains(r, "list of hosts"));
    CHECK(!contains(r, "Tonight"));

    // The contact form mails the owner (printed, in dev mode).
    b.get("/contact");
    r = b.post("/contact/send", "from=ann%40example.com&message=Hello%20there");
    CHECK(statusOf(r) == 303);
    CHECK(contains(s.out.str(), "owner@example.com"));
    CHECK(contains(s.out.str(), "Hello there"));
    CHECK(contains(bodyOf(b.get("/contact")), "Thanks"));

    // The webhook, signed with the sample's dev key.
    Browser server(s.port());
    std::string body = "{\"event\":\"deploy\"}";
    std::string sig = toHex(hmacSha256("dev-only-hook-key", body));
    r = server.send("POST", "/hooks/deploy", body, false, "X-Signature: sha256=" + sig + "\r\n", "application/json");
    CHECK(statusOf(r) == 200);
    r = server.send("POST", "/hooks/deploy", body, false, "X-Signature: sha256=00\r\n", "application/json");
    CHECK(statusOf(r) == 401);
}

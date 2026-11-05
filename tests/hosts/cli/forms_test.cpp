// End to end (M7): declared forms, error redisplay, uploads, rate limits,
// the timing check, proof of work and trusted proxies, through `fourfold
// dev`'s server on a real socket.
//
// The exit criteria (milestones.md, M7): the guestbook with a declared form,
// error redisplay and spam rules; tampered submissions (extra fields, bad
// choices, forged file types) rejected or dropped; a flood throttled as
// configured, including behind a trusted proxy (visitors told apart by
// forwarded address; a forged X-Forwarded-For from an untrusted peer
// ignored).
#include <filesystem>
#include <sstream>
#include <thread>

#include "app_server.h"
#include "base/crypto.h"
#include "browser.h"
#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::test::bodyOf;
using fourfold::test::Browser;
using fourfold::test::TempDir;
using fourfold::test::textBetween;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int statusOf(const std::string& response) {
    return response.size() > 12 ? std::atoi(response.c_str() + 9) : 0;
}

std::string header(const std::string& response, const std::string& name) {
    std::string head = response.substr(0, response.find("\r\n\r\n"));
    return textBetween(head, "\r\n" + name + ": ", "\r\n");
}

// "a=1&b=x y" with each value percent-encoded.
std::string encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    for (const auto& [name, value] : fields) {
        if (!out.empty()) out += "&";
        out += name + "=";
        for (unsigned char c : value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += kHex[c >> 4];
                out += kHex[c & 15];
            }
        }
    }
    return out;
}

std::string forwardedFor(const std::string& address) { return "X-Forwarded-For: " + address + "\r\n"; }

struct Server {
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;

    explicit Server(const std::string& root) {
        server = AppServer::startDev(root, 0, out, err);
        REQUIRE_MESSAGE(server, err.str());
        REQUIRE_MESSAGE(server->compiled(), err.str());
        serving = std::thread([this] { server->loop().run(); });
    }
    ~Server() {
        server->loop().stop();
        serving.join();
    }
    uint16_t port() const { return server->port(); }
};

// The guestbook sample, copied out of the source tree, with a config that
// trusts the test's own address as a proxy.
std::string copyGuestbook(const TempDir& dir, const std::string& appMs) {
    namespace fs = std::filesystem;
    fs::path from = fs::path(FOURFOLD_TESTS_DIR) / ".." / "examples" / "guestbook" / "code";
    fs::path to = dir.at("guestbook");
    fs::create_directories(to / "code");
    fs::copy(from, to / "code", fs::copy_options::recursive);
    dir.write("guestbook/private/config/app.ms", appMs);
    return to.string();
}

const char* const kLab =
    "form.echo = {\n"
    "    \"name\": {\"type\": \"text\", \"required\": true, \"max\": 10},\n"
    "    \"n\":    {\"type\": \"int\"},\n"
    "    \"pick\": {\"type\": \"choice\", \"options\": [\"a\", \"b\"]},\n"
    "    \"many\": {\"type\": \"choices\", \"options\": [\"x\", \"y\"]},\n"
    "    \"ok\":   {\"type\": \"bool\"},\n"
    "}\n"
    "post.echo = function(req)\n"
    "    return json({\"form\": req.form, \"errors\": req.errors, \"files\": req.files.len})\n"
    "end function\n"
    "post.raw = function(req)\n"
    "    return json(req.form)\n"
    "end function\n"
    "form.up = {\n"
    "    \"pic\":   {\"type\": \"file\", \"required\": true, \"maxKB\": 4, \"types\": [\"image/png\"]},\n"
    "    \"title\": {\"type\": \"text\"},\n"
    "}\n"
    "post.up = function(req)\n"
    "    if req.errors then return status(422, json(req.errors))\n"
    "    p = req.files.pic\n"
    "    file.makedir \"pics\"\n"
    "    file.saveRaw \"pics/\" + p.name, p.data\n"
    "    return json({\"name\": p.name, \"type\": p.type, \"size\": p.size, \"title\": req.form.title})\n"
    "end function\n"
    "get.pic = function(req)\n"
    "    return file.send(\"pics/\" + req.query.name)\n"
    "end function\n"
    "get.dl = function(req)\n"
    "    return file.send(\"pics/\" + req.query.name, \"photo \" + char(233) + \".png\")\n"
    "end function\n"
    "get.page = function(req)\n"
    "    return file.send(\"evil.html\")\n"
    "end function\n"
    "get.ip = function(req)\n"
    "    return req.ip\n"
    "end function\n"
    "post.counted = function(req)\n"
    "    if not ratelimit(\"thing\", \"2/minute\") then return status(429, \"slow down\")\n"
    "    return \"ok\"\n"
    "end function\n"
    "post.slow = function(req)\n"
    "    return \"slow ok\"\n"
    "end function\n"
    "post.pow = function(req)\n"
    "    return \"pow ok\"\n"
    "end function\n";

const char* const kLabPage =
    "<!DOCTYPE html><html><head><title>Lab</title></head><body>"
    "<form method=\"post\" action=\"/lab/echo\"><input name=\"name\"></form>"
    "<form method=\"post\" action=\"/lab/pow\"><input name=\"x\"></form>"
    "</body></html>";

std::string writeLab(const TempDir& dir, const std::string& appMs) {
    dir.write("lab/code/pages/lab.ms", kLab);
    dir.write("lab/code/pages/lab.html", kLabPage);
    dir.write("lab/code/limits.ms",
              "limits = {}\n"
              "limits[\"/lab/slow\"] = {\"minSeconds\": 1}\n"
              "limits[\"/lab/pow\"] = {\"challenge\": true}\n");
    dir.write("lab/private/config/app.ms", appMs);
    dir.write("lab/data/evil.html", "<script>alert(1)</script>");
    return dir.at("lab");
}

std::string png(size_t extra = 0) { return std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16) + std::string(extra, 'p'); }

std::string multipart(const std::vector<std::tuple<std::string, std::string, std::string>>& parts) {
    std::string body;
    for (const auto& [name, filename, content] : parts) {
        body += "--B0undary\r\nContent-Disposition: form-data; name=\"" + name + "\"";
        if (!filename.empty()) body += "; filename=\"" + filename + "\"\r\nContent-Type: image/png";
        body += "\r\n\r\n" + content + "\r\n";
    }
    return body + "--B0undary--\r\n";
}

// Solves a puzzle the way the browser's solver does.
std::string solve(const std::string& puzzle) {
    std::string afterTwo = puzzle.substr(puzzle.find('.', puzzle.find('.') + 1) + 1);
    int bits = std::atoi(afterTwo.c_str());
    for (int n = 0;; n++) {
        std::string answer = puzzle + "~" + std::to_string(n);
        if (leadingZeroBits(sha256(answer)) >= bits) return answer;
    }
}

}  // namespace

TEST_CASE("forms: the guestbook, with a declared form, error redisplay and spam rules") {
    TempDir dir;
    Server s(copyGuestbook(dir, "config = {}\nconfig.proxy = {\"trust\": [\"127.0.0.1\"]}\n"));
    Browser b(s.port());
    std::string page = b.get("/");
    REQUIRE_EQ(statusOf(page), 200);
    // The engine added the declaration's rules to the inputs.
    CHECK(contains(page, "<input name=\"who\" value=\"\" placeholder=\"Your name\" required maxlength=\"40\">"));
    REQUIRE_FALSE(b.formTime().empty());
    std::string t = b.formTime();

    // The photos page's upload form is a fragment (frag("uploadForm")) with a
    // puzzle: the page around it must still link the solver.
    std::string photos = Browser(s.port()).get("/photos");
    CHECK(contains(photos, "data-ff-challenge=\""));
    CHECK(contains(photos, "<script src=\"/__fourfold/challenge.js\""));
    CHECK_LT(photos.find("/__fourfold/challenge.js"), photos.find("</head>"));

    // Too quick: minSeconds is 2 in code/limits.ms.
    std::string quick = b.post("/sign", encode({{"_t", t}, {"who", "Ann"}, {"msg", "Hi"}}));
    CHECK_EQ(statusOf(quick), 429);
    CHECK(contains(bodyOf(quick), "That was quick"));
    CHECK_FALSE(header(quick, "Retry-After").empty());
    // No timing token at all (a bot posting directly): refused.
    CHECK_EQ(statusOf(b.post("/sign", encode({{"who", "Bot"}, {"msg", "spam"}}))), 403);

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    SUBCASE("errors with HTMX: the form comes back in place, 422") {
        std::string r = b.post("/sign", encode({{"_t", t}, {"who", "  "}, {"msg", "http://a.example http://b.example "
                                                                                  "http://c.example"},
                                                 {"mood", "meh"}, {"role", "admin"}}),
                               true);
        CHECK_EQ(statusOf(r), 422);
        CHECK_EQ(header(r, "HX-Retarget"), "#signForm");
        CHECK_EQ(header(r, "HX-Reswap"), "outerHTML");
        std::string form = bodyOf(r);
        CHECK(contains(form, "<form id=\"signForm\""));
        CHECK(contains(form, "Please sign your name."));
        CHECK(contains(form, "Please include no more than 2 links."));
        CHECK(contains(form, "http://a.example http://b.example http://c.example</textarea>"));   // as typed
        CHECK(contains(form, "value=\"meh\" checked"));
        CHECK(contains(form, "name=\"_csrf\""));   // a fresh form, still postable

        // A check only the handler makes.
        std::string taken = b.post("/sign", encode({{"_t", t}, {"who", "Admin"}, {"msg", "hello"}}), true);
        CHECK_EQ(statusOf(taken), 422);
        CHECK(contains(bodyOf(taken), "That name is reserved."));
        CHECK(contains(bodyOf(taken), "value=\"Admin\""));
    }

    SUBCASE("errors without JavaScript: back to the page, which shows them once") {
        std::string r = b.post("/sign", encode({{"_t", t}, {"who", std::string(50, 'x')}, {"msg", "My <b>message</b>"}}));
        CHECK_EQ(statusOf(r), 303);
        CHECK_EQ(header(r, "Location"), "/");
        std::string again = b.get("/");
        CHECK(contains(again, "Please use at most 40 characters."));
        CHECK(contains(again, "value=\"" + std::string(50, 'x') + "\""));
        CHECK(contains(again, "My &lt;b&gt;message&lt;/b&gt;</textarea>"));   // escaped like any value
        std::string third = b.get("/");
        CHECK_FALSE(contains(third, "Please use at most 40 characters."));   // shown once
        // Another visitor never sees them.
        Browser other(s.port());
        CHECK_FALSE(contains(other.get("/"), "Please use at most"));
    }

    SUBCASE("a good entry, then a flood: 5 a minute, per visitor address") {
        // Each visitor comes through the trusted proxy (the test's own
        // address), which says who they are.
        const std::string ann = forwardedFor("198.51.100.10");
        std::string ok = b.send("POST", "/sign", encode({{"_t", t}, {"who", "Ann"}, {"msg", "Hi *there*"},
                                                          {"mood", "happy"}, {"shout", "on"}}),
                                true, ann);
        REQUIRE_EQ(statusOf(ok), 200);
        CHECK(contains(bodyOf(ok), "<b>Ann</b> <span class=\"mood\">happy</span>"));
        CHECK(contains(bodyOf(ok), "HI <em>THERE</em>"));
        for (int i = 0; i < 4; i++) {
            CHECK_EQ(statusOf(b.send("POST", "/sign", encode({{"_t", t}, {"who", "Ann"}, {"msg", "again"}}), true, ann)),
                     200);
        }
        std::string flooded = b.send("POST", "/sign", encode({{"_t", t}, {"who", "Ann"}, {"msg", "more"}}), true, ann);
        CHECK_EQ(statusOf(flooded), 429);
        CHECK(contains(bodyOf(flooded), "Slow down a little"));   // the app's pages/429.html
        int retry = std::atoi(header(flooded, "Retry-After").c_str());
        CHECK_GE(retry, 1);
        CHECK_LE(retry, 60);
        // Another visitor, by forwarded address, has a count of their own...
        std::string other = b.send("POST", "/sign", encode({{"_t", t}, {"who", "Bo"}, {"msg", "hi"}}), true,
                                   forwardedFor("198.51.100.20"));
        CHECK_EQ(statusOf(other), 200);
        // ...and the first can't escape theirs by putting another address at
        // the left of the header: the proxy adds the real one at the right.
        std::string forged = b.send("POST", "/sign", encode({{"_t", t}, {"who", "Ann"}, {"msg", "x"}}), true,
                                    forwardedFor("203.0.113.66, 198.51.100.10"));
        CHECK_EQ(statusOf(forged), 429);
    }
}

TEST_CASE("forms: tampered submissions, and forms without a declaration") {
    TempDir dir;
    Server s(writeLab(dir, "config = {}\n"));
    Browser b(s.port());
    REQUIRE_EQ(statusOf(b.get("/lab")), 200);

    std::string r = b.post("/lab/echo", encode({{"name", " Eve "}, {"n", "7"}, {"pick", "z"}, {"many", "x"},
                                                {"many", "q"}, {"many", "y"}, {"isAdmin", "1"}, {"price", "0"},
                                                {"ok", "on"}}));
    REQUIRE_EQ(statusOf(r), 200);
    std::string body = bodyOf(r);
    CHECK(contains(body, "\"name\":\"Eve\""));
    CHECK(contains(body, "\"n\":7"));                 // typed
    CHECK(contains(body, "\"pick\":null"));           // not an option: dropped
    CHECK(contains(body, "\"many\":[\"x\",\"y\"]"));  // only the options
    CHECK(contains(body, "\"ok\":1"));
    CHECK_FALSE(contains(body, "isAdmin"));           // undeclared: never reaches the handler
    CHECK_FALSE(contains(body, "price"));
    CHECK(contains(body, "\"errors\":null"));

    std::string bad = bodyOf(b.post("/lab/echo", encode({{"name", ""}, {"n", "1e400"}})));
    CHECK(contains(bad, "\"name\":\"Please fill this in.\""));
    CHECK(contains(bad, "\"n\":\"Please enter a whole number.\""));
    CHECK(contains(bad, "\"n\":null"));

    // A handler with no declaration gets raw strings (bounded by the limits).
    CHECK(contains(bodyOf(b.post("/lab/raw", encode({{"anything", "goes"}}))), "\"anything\":\"goes\""));
}

TEST_CASE("forms: uploads checked by content, saved, and sent back safely") {
    TempDir dir;
    Server s(writeLab(dir, "config = {}\n"));
    Browser b(s.port());
    REQUIRE_EQ(statusOf(b.get("/lab")), 200);
    const std::string type = "multipart/form-data; boundary=B0undary";

    std::string ok = b.send("POST", "/lab/up", multipart({{"pic", "..\\..\\cat.png", png(10)}, {"title", "", " Cat "}}),
                            false, "", type);
    REQUIRE_MESSAGE(statusOf(ok) == 200, ok);
    CHECK(contains(bodyOf(ok), "\"name\":\"cat.png\""));
    CHECK(contains(bodyOf(ok), "\"type\":\"image/png\""));
    CHECK(contains(bodyOf(ok), "\"size\":26"));
    CHECK(contains(bodyOf(ok), "\"title\":\"Cat\""));

    // A forged type: HTML with a .png name and an image/png claim.
    std::string forged = b.send("POST", "/lab/up", multipart({{"pic", "x.png", "<script>alert(1)</script>"}}), false, "",
                                type);
    CHECK_EQ(statusOf(forged), 422);
    CHECK(contains(bodyOf(forged), "Please choose a file of the right type (PNG)."));
    CHECK_EQ(statusOf(b.send("POST", "/lab/up", multipart({{"pic", "big.png", png(5000)}}), false, "", type)), 422);
    CHECK_EQ(statusOf(b.send("POST", "/lab/up", multipart({{"title", "", "no file"}}), false, "", type)), 422);
    // Over the route's limit for uploads altogether: refused before parsing.
    std::string huge = b.send("POST", "/lab/up", multipart({{"pic", "huge.png", png(2 * 1024 * 1024)}}), false, "", type);
    CHECK_EQ(statusOf(huge), 413);

    // Sent back: a PNG inline, as a PNG; a download when asked; an HTML file
    // in data/ never as HTML.
    std::string shown = b.get("/lab/pic?name=cat.png");
    REQUIRE_EQ(statusOf(shown), 200);
    CHECK_EQ(header(shown, "Content-Type"), "image/png");
    CHECK_EQ(header(shown, "Content-Disposition"), "inline; filename=\"cat.png\"; filename*=UTF-8''cat.png");
    CHECK_EQ(header(shown, "X-Content-Type-Options"), "nosniff");
    CHECK_EQ(bodyOf(shown), png(10));
    std::string download = b.get("/lab/dl?name=cat.png");
    INFO(header(download, "Content-Disposition"));
    CHECK(contains(header(download, "Content-Disposition"), "attachment; filename=\"photo _.png\"; "
                                                            "filename*=UTF-8''photo%20%C3%A9.png"));
    std::string evil = b.get("/lab/page");
    CHECK_EQ(header(evil, "Content-Type"), "application/octet-stream");
    CHECK(contains(header(evil, "Content-Disposition"), "attachment"));
    CHECK_NE(statusOf(b.get("/lab/pic?name=../lab.ms")), 200);   // file can't leave data/
}

TEST_CASE("forms: the timing check, proof of work, and ratelimit() from code") {
    TempDir dir;
    Server s(writeLab(dir, "config = {}\n"));
    Browser b(s.port());
    std::string page = b.get("/lab");
    REQUIRE_EQ(statusOf(page), 200);
    std::string t = b.formTime();

    // minSeconds 1 on /lab/slow.
    CHECK_EQ(statusOf(b.post("/lab/slow", encode({{"_t", t}}))), 429);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK_EQ(bodyOf(b.post("/lab/slow", encode({{"_t", t}}))), "slow ok");

    // Only the form that posts to the challenge route carries a puzzle, and
    // the page links the solver.
    CHECK(contains(page, "<script src=\"/__fourfold/challenge.js\""));
    REQUIRE_FALSE(b.puzzle().empty());
    std::string noAnswer = b.post("/lab/pow", encode({{"x", "1"}}));
    CHECK_EQ(statusOf(noAnswer), 403);
    CHECK(contains(bodyOf(noAnswer), "needs JavaScript"));
    std::string answer = solve(b.puzzle());   // dev mode: 4 bits, quick
    CHECK_EQ(bodyOf(b.post("/lab/pow", encode({{"_pow", answer}, {"x", "1"}}))), "pow ok");
    CHECK_EQ(statusOf(b.post("/lab/pow", encode({{"_pow", answer}, {"x", "2"}}))), 403);   // once only

    // The solver is served on its own and with htmx.
    CHECK(contains(b.get("/__fourfold/challenge.js"), "fourfoldChallenge"));
    CHECK(contains(b.get("/__fourfold/htmx.js"), "fourfoldChallenge"));

    // ratelimit("thing", "2/minute") in post.counted.
    CHECK_EQ(bodyOf(b.post("/lab/counted", "")), "ok");
    CHECK_EQ(bodyOf(b.post("/lab/counted", "")), "ok");
    CHECK_EQ(bodyOf(b.post("/lab/counted", "")), "slow down");
}

TEST_CASE("forms: a flood, throttled by the default limits; X-Forwarded-For trusted only from a proxy") {
    SUBCASE("behind a trusted proxy: visitors told apart by forwarded address") {
        TempDir dir;
        Server s(writeLab(dir, "config = {}\nconfig.limits = {\"anonymous\": \"5/minute\"}\n"
                               "config.proxy = {\"trust\": [\"127.0.0.1\"]}\n"));
        Browser b(s.port());
        REQUIRE_EQ(statusOf(b.get("/lab")), 200);
        CHECK_EQ(bodyOf(b.send("GET", "/lab/ip", "", false, forwardedFor("198.51.100.7"))), "198.51.100.7");
        for (int i = 0; i < 5; i++) {
            CHECK_EQ(statusOf(b.send("POST", "/lab/raw", "", false, forwardedFor("198.51.100.7"))), 200);
        }
        std::string refused = b.send("POST", "/lab/raw", "", false, forwardedFor("198.51.100.7"));
        CHECK_EQ(statusOf(refused), 429);
        CHECK(contains(bodyOf(refused), "Too many requests"));
        CHECK_FALSE(header(refused, "Retry-After").empty());
        // GETs aren't counted by the defaults.
        CHECK_EQ(statusOf(b.send("GET", "/lab/ip", "", false, forwardedFor("198.51.100.7"))), 200);
        // Another visitor, through the same proxy.
        CHECK_EQ(statusOf(b.send("POST", "/lab/raw", "", false, forwardedFor("198.51.100.8"))), 200);
        // An IPv6 visitor is counted by their /64.
        for (int i = 0; i < 5; i++) {
            std::string address = "2001:db8:1:2::" + std::to_string(i + 1);
            CHECK_EQ(statusOf(b.send("POST", "/lab/raw", "", false, forwardedFor(address))), 200);
        }
        CHECK_EQ(statusOf(b.send("POST", "/lab/raw", "", false, forwardedFor("2001:db8:1:2::99"))), 429);
        CHECK_EQ(statusOf(b.send("POST", "/lab/raw", "", false, forwardedFor("2001:db8:1:3::1"))), 200);
    }
    SUBCASE("not from a trusted proxy: a forged X-Forwarded-For is ignored") {
        TempDir dir;
        Server s(writeLab(dir, "config = {}\nconfig.limits = {\"anonymous\": \"5/minute\"}\n"
                               "config.proxy = {\"trust\": [\"10.0.0.0/8\"]}\n"));
        Browser b(s.port());
        REQUIRE_EQ(statusOf(b.get("/lab")), 200);
        CHECK_EQ(bodyOf(b.send("GET", "/lab/ip", "", false, forwardedFor("198.51.100.7"))), "127.0.0.1");
        int allowed = 0;
        for (int i = 0; i < 10; i++) {
            std::string forged = forwardedFor("198.51.100." + std::to_string(i));
            if (statusOf(b.send("POST", "/lab/raw", "", false, forged)) == 200) allowed++;
        }
        CHECK_EQ(allowed, 5);
    }
}

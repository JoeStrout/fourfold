// End to end: sessions, flash, data and CSRF through `fourfold dev`'s server,
// on a real socket, with an app in a temporary folder.
#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "app_server.h"
#include "browser.h"
#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::test::bodyOf;
using fourfold::test::Browser;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The test app.
void writeApp(const TempDir& dir) {
    dir.write("app/code/pages/counter.html", "<!DOCTYPE html><html><body><p>{{visits}}</p></body></html>");
    dir.write("app/code/pages/counter.ms",
              "render = function(req)\n"
              "    return {\"visits\": data.increment(\"visits\")}\n"
              "end function\n"
              "get.bump = function(req)\n"
              "    return str(data.increment(\"visits\"))\n"
              "end function\n");

    dir.write("app/code/pages/cart.html",
              "<!DOCTYPE html><html><body><p class=flash>{{flash}}</p><p>{{n}}</p><ul>{{items}}</ul>"
              "<form method=post action=/cart/add><input name=item></form></body></html>");
    dir.write("app/code/pages/cart.ms",
              "render = function(req)\n"
              "    f = {\"n\": 0, \"items\": \"\"}\n"
              "    if session.hasIndex(\"n\") then f.n = session.n\n"
              "    if session.hasIndex(\"items\") then f.items = session.items.join(\",\")\n"
              "    return f\n"
              "end function\n"
              "get.count = function(req)\n"
              "    if not session.hasIndex(\"n\") then session.n = 0\n"
              "    session.n += 1\n"
              "    return str(session.n)\n"
              "end function\n"
              "post.add = function(req)\n"
              "    if not session.hasIndex(\"items\") then session.items = []\n"
              "    session.items.push req.form.item\n"
              "    session.flash = \"Added \" + req.form.item\n"
              "    return redirect(\"/cart\")\n"
              "end function\n"
              "get.big = function(req)\n"
              "    session.big = \"x\" * 70000\n"
              "    return \"too big\"\n"
              "end function\n"
              "get.fn = function(req)\n"
              "    session.f = @render\n"
              "    return \"a function\"\n"
              "end function\n"
              "get.peek = function(req)\n"
              "    return json(session)\n"
              "end function\n");
    // A page with no code shows the flash too.
    dir.write("app/code/pages/plain.html", "<!DOCTYPE html><html><body><p class=flash>{{flash}}</p></body></html>");
    dir.write("app/code/pages/flasher.ms",
              "get.go = function(req)\n"
              "    session.flash = \"Hello from flasher\"\n"
              "    return redirect(\"/plain\")\n"
              "end function\n"
              "get.rich = function(req)\n"
              "    session.flash = {\"text\": \"Saved.\", \"kind\": \"ok\"}\n"
              "    return redirect(\"/notice\")\n"
              "end function\n"
              "get.peek = function(req)\n"
              "    if session.hasIndex(\"flash\") then return \"waiting: \" + session.flash\n"
              "    return \"none\"\n"
              "end function\n");
    // A message can be a map, taken apart by the template; and render can
    // fill {{flash}} itself, which still uses up the waiting message.
    dir.write("app/code/pages/notice.html",
              "<!DOCTYPE html><html><body><p class=\"flash {{flash.kind}}\">{{flash.text}}</p></body></html>");
    dir.write("app/code/pages/own.html", "<!DOCTYPE html><html><body><p class=flash>{{flash}}</p></body></html>");
    dir.write("app/code/pages/own.ms",
              "render = function(req)\n"
              "    return {\"flash\": \"computed\"}\n"
              "end function\n");

    dir.write("app/code/pages/book.ms",
              "post.sign = function(req)\n"
              "    data.append \"book\", {\"who\": req.form.who}\n"
              "    return redirect(\"/\")\n"
              "end function\n"
              "get.list = function(req)\n"
              "    return json(data.get(\"book\", []))\n"
              "end function\n");
    dir.write("app/code/pages/upd.ms",
              "addOne = function(x)\n"
              "    return x + 1\n"
              "end function\n"
              "get.go = function(req)\n"
              "    return str(data.update(\"u\", @addOne, 41))\n"
              "end function\n"
              "get.nested = function(req)\n"
              "    outerF = function(a)\n"
              "        data.update \"b\", @addOne, 0\n"
              "        return a + 10\n"
              "    end function\n"
              "    r = data.update(\"a\", @outerF, 0)\n"
              "    return str(r) + \",\" + str(data.get(\"b\"))\n"
              "end function\n"
              "get.same = function(req)\n"
              "    inner = function(a)\n"
              "        return data.update(\"s\", @addOne, 0)\n"
              "    end function\n"
              "    r = data.update(\"s\", @inner, 0)\n"
              "    return str(r)\n"
              "end function\n"
              "get.fail = function(req)\n"
              "    bad = function(a)\n"
              "        return @addOne\n"
              "    end function\n"
              "    r = data.update(\"u\", @bad, 0)\n"
              "    return \"result: \" + str(r)\n"
              "end function\n"
              "get.misc = function(req)\n"
              "    data.set \"m/one\", [1, \"two\"]\n"
              "    r = []\n"
              "    r.push data.exists(\"m/one\")\n"
              "    r.push data.compareAndSet(\"m/one\", [1, \"two\"], 3)\n"
              "    r.push data.compareAndSet(\"m/one\", [1, \"two\"], 4)\n"
              "    r.push data.get(\"m/one\")\n"
              "    r.push data.names(\"m\")\n"
              "    r.push data.delete(\"m/one\")\n"
              "    r.push data.get(\"m/one\", \"gone\")\n"
              "    e = data.get(\"../x\")\n"
              "    r.push e isa error\n"
              "    for i in r.indexes\n"
              "        if r[i] isa error then r[i] = \"error: \" + r[i].message\n"
              "    end for\n"
              "    r.push e.message\n"
              "    return json(r)\n"
              "end function\n");
    dir.write("app/code/pages/log.ms",
              "get.write = function(req)\n"
              "    dropbox.write \"app.log\", \"hello \" + req.query.who\n"
              "    return \"ok\"\n"
              "end function\n");
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>home</body></html>");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Server {
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;

    explicit Server(const TempDir& dir) {
        server = AppServer::startDev(dir.at("app"), 0, out, err);
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

}  // namespace

TEST_CASE("State: the counter and the guestbook persist across restarts") {
    TempDir dir;
    writeApp(dir);
    {
        Server s(dir);
        Browser b(s.port());
        CHECK(contains(b.get("/counter"), "<p>1</p>"));
        CHECK(contains(b.get("/counter"), "<p>2</p>"));
        b.get("/");   // for a CSRF token
        CHECK(contains(b.post("/book/sign", "who=Ada"), "303 See Other"));
        CHECK(contains(b.post("/book/sign", "who=%3Cb%3E"), "303 See Other"));
    }
    Server s(dir);
    Browser b(s.port());
    CHECK(contains(b.get("/counter"), "<p>3</p>"));
    CHECK_EQ(bodyOf(b.get("/book/list")), "[{\"who\":\"Ada\"},{\"who\":\"\\u003cb\\u003e\"}]");
    CHECK_EQ(slurp(dir.at("app/data/visits.json")), "3");
}

TEST_CASE("State: sessions begin on first write, persist, and carry flash messages") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    // Reading the session doesn't create one.
    std::string page = b.get("/cart");
    CHECK(contains(page, "200 OK"));
    CHECK_FALSE(b.cookies().count("ff_session"));
    CHECK(b.cookies().count("ff_csrf"));
    // Writing does.
    CHECK_EQ(bodyOf(b.get("/cart/count")), "1");
    REQUIRE(b.cookies().count("ff_session"));
    std::string id = b.cookies().at("ff_session");
    CHECK_EQ(bodyOf(b.get("/cart/count")), "2");
    CHECK_EQ(b.cookies().at("ff_session"), id);   // the same session

    b.get("/cart");
    CHECK(contains(b.post("/cart/add", "item=apple"), "303 See Other"));
    std::string after = b.get("/cart");
    CHECK(contains(after, "<p class=flash>Added apple</p>"));
    CHECK(contains(after, "<p>2</p><ul>apple</ul>"));
    CHECK(contains(b.get("/cart"), "<p class=flash></p>"));   // shown once

    // A page with no code shows (and clears) the flash too.
    b.get("/flasher/go");
    CHECK(contains(b.get("/plain"), "<p class=flash>Hello from flasher</p>"));
    CHECK(contains(b.get("/plain"), "<p class=flash></p>"));

    // Code can read a waiting message; reading doesn't use it up.
    b.get("/flasher/go");
    CHECK_EQ(bodyOf(b.get("/flasher/peek")), "waiting: Hello from flasher");
    CHECK_EQ(bodyOf(b.get("/flasher/peek")), "waiting: Hello from flasher");
    CHECK(contains(b.get("/plain"), "<p class=flash>Hello from flasher</p>"));
    CHECK_EQ(bodyOf(b.get("/flasher/peek")), "none");

    // A map message, shown through dotted fields (on a page with no code).
    b.get("/flasher/rich");
    CHECK(contains(b.get("/notice"), "<p class=\"flash ok\">Saved.</p>"));
    CHECK(contains(b.get("/notice"), "<p class=\"flash \"></p>"));

    // render's own flash field wins, and the waiting message is gone.
    b.get("/flasher/go");
    CHECK(contains(b.get("/own"), "<p class=flash>computed</p>"));
    CHECK_EQ(bodyOf(b.get("/flasher/peek")), "none");

    // Another browser has its own (none).
    Browser other(s.port());
    CHECK_EQ(bodyOf(other.get("/cart/peek")), "{}");
    // The app never sees the engine's cookies.
    CHECK(contains(bodyOf(b.get("/cart/peek")), "\"n\":2"));
}

TEST_CASE("State: a session that can't be saved is a 500, and nothing is saved") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    CHECK_EQ(bodyOf(b.get("/cart/count")), "1");
    std::string big = b.get("/cart/big");
    CHECK(contains(big, "500 Internal Server Error"));
    CHECK(contains(big, "over its limit"));
    CHECK(contains(big, "big ("));   // names the largest key
    std::string fn = b.get("/cart/fn");
    CHECK(contains(fn, "500 Internal Server Error"));
    CHECK(contains(fn, "contains a function"));
    CHECK_EQ(bodyOf(b.get("/cart/peek")), "{\"n\":1}");
}

TEST_CASE("State: CSRF") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    b.get("/cart");
    // No token, a wrong token, another site's origin: refused before the handler.
    Browser bare(s.port());
    CHECK(contains(bare.post("/book/sign", "who=x"), "403 Forbidden"));
    std::string base = "POST /book/sign HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                       "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 5\r\n";
    std::string cookie = "Cookie: ff_csrf=" + b.cookies().at("ff_csrf") + "\r\n";
    CHECK(contains(test::rawFetch(s.port(), base + cookie + "X-CSRF-Token: nope\r\n\r\nwho=x"), "403 Forbidden"));
    std::string page = b.get("/cart");
    std::string token = test::textBetween(page, "name=\"_csrf\" value=\"", "\"");
    REQUIRE_FALSE(token.empty());
    CHECK(contains(test::rawFetch(s.port(), base + cookie + "Origin: https://evil.example\r\n\r\nwho=x&_csrf=" + token),
                   "403 Forbidden"));
    // The form's own field works, and so does the header.
    std::string fieldBase = "POST /book/sign HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                            "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                            std::to_string(12 + token.size()) + "\r\n";
    CHECK(contains(test::rawFetch(s.port(), fieldBase + cookie + "Origin: http://localhost\r\n\r\nwho=x&_csrf=" + token),
                   "303 See Other"));
    CHECK(contains(b.post("/book/sign", "who=y"), "303 See Other"));
    CHECK_EQ(bodyOf(b.get("/book/list")), "[{\"who\":\"x\"},{\"who\":\"y\"}]");   // _csrf never reached the app
    CHECK(contains(slurp(dir.at("app/private/state/logs/" +
                                std::string(s.server->log().currentFile()).substr(5))),
                   "csrf POST /book/sign"));
}

TEST_CASE("State: data.update, nested updates, and the other data functions") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    CHECK_EQ(bodyOf(b.get("/upd/go")), "42");
    CHECK_EQ(bodyOf(b.get("/upd/go")), "43");
    CHECK_EQ(bodyOf(b.get("/upd/nested")), "10,1");
    CHECK(contains(bodyOf(b.get("/upd/same")), "would wait for itself"));
    CHECK(contains(bodyOf(b.get("/upd/fail")), "result: "));
    CHECK(contains(bodyOf(b.get("/upd/fail")), "function"));
    CHECK_EQ(bodyOf(b.get("/upd/go")), "44");   // the failed updates changed nothing, and freed the lock
    std::string misc = bodyOf(b.get("/upd/misc"));
    INFO(misc);
    CHECK(contains(misc, "[1,1,0,3,[\"one\"],1,\"gone\",1,"));
    CHECK(contains(misc, "data.get: bad data name"));   // the bad name's error
    b.get("/log/write?who=Ada");
    CHECK_EQ(slurp(dir.at("app/dropbox/app.log")), "hello Ada\n");
}

TEST_CASE("State: many clients at once lose no updates") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    uint16_t port = s.port();

    // Many clients bumping one counter.
    constexpr int kThreads = 8, kEach = 25;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([port, &failures] {
            for (int i = 0; i < kEach; i++) {
                std::string r = test::rawFetch(port, "GET /counter/bump HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
                if (r.find("200 OK") == std::string::npos) failures++;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);
    Browser b(port);
    CHECK_EQ(bodyOf(b.get("/counter/bump")), std::to_string(kThreads * kEach + 1));

    // One session, from several tabs at once.
    CHECK_EQ(bodyOf(b.get("/cart/count")), "1");
    std::string cookie = "Cookie: ff_session=" + b.cookies().at("ff_session") + "\r\n";
    threads.clear();
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([port, cookie, &failures] {
            for (int i = 0; i < kEach; i++) {
                std::string r = test::rawFetch(port, "GET /cart/count HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
                                                         cookie + "\r\n");
                if (r.find("200 OK") == std::string::npos) failures++;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(bodyOf(b.get("/cart/count")), std::to_string(kThreads * kEach + 2));

    // Nothing left behind: no locks held or waited for, no runs, and no
    // bundles beyond the current app's.
    auto sessionLocks = s.server->sessionLockStats();
    auto dataLocks = s.server->dataLockStats();
    CHECK_EQ(sessionLocks.held + sessionLocks.waiting, 0u);
    CHECK_EQ(dataLocks.held + dataLocks.waiting, 0u);
    ScriptHost::Stats stats = s.server->scriptStats();
    CHECK_EQ(stats.activeRuns, 0u);
    CHECK_EQ(stats.liveBundles, 1);
}

TEST_CASE("State: a run that ends while holding locks lets them go") {
    TempDir dir;
    writeApp(dir);
    dir.write("app/code/pages/stuck.ms",
              "forever = function(x)\n"
              "    while true\n"
              "    end while\n"
              "end function\n"
              "get.update = function(req)\n"
              "    data.update \"u\", @forever, 0\n"
              "end function\n"
              "get.session = function(req)\n"
              "    session.x = 1\n"
              "    while true\n"
              "    end while\n"
              "end function\n");
    Server s(dir);
    Browser b(s.port());
    CHECK_EQ(bodyOf(b.get("/cart/count")), "1");
    // Each runs past its instruction budget, holding a data lock or the session's.
    CHECK(contains(b.get("/stuck/update"), "500 Internal Server Error"));
    CHECK(contains(b.get("/stuck/session"), "500 Internal Server Error"));
    // The locks are free again: the same data name and the same session work.
    CHECK_EQ(bodyOf(b.get("/upd/go")), "42");
    CHECK_EQ(bodyOf(b.get("/cart/count")), "2");   // and the failed run's change wasn't saved
    CHECK_EQ(bodyOf(b.get("/cart/peek")), "{\"n\":2}");
    auto dataLocks = s.server->dataLockStats();
    CHECK_EQ(dataLocks.held + dataLocks.waiting, 0u);
}

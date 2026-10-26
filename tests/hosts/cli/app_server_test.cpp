// End to end: `fourfold dev`'s server, on a real socket, serving
// tests/apps/scripts with its code running on the script thread.
#include "app_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;

namespace {

// One request over a fresh blocking connection; the whole response, or ""
// if this environment won't allow connecting.
std::string fetch(uint16_t port, const std::string& request) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    Fd client(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    // Claude Code's macOS sandbox sometimes refuses a process's first
    // loopback connections (EPERM) for a moment (see http_server_test.cpp).
    for (int tries = 0; connected < 0 && errno == EPERM && tries < 5; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    }
    INFO("connect: " << (connected == 0 ? "ok" : std::strerror(errno)));
    REQUIRE(connected == 0);
    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
        REQUIRE(n > 0);
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<size_t>(n));
    }
    return response;
}

// A connected blocking socket to localhost:port.
Fd connectTo(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    Fd client(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    for (int tries = 0; connected < 0 && errno == EPERM && tries < 5; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    }
    REQUIRE(connected == 0);
    return client;
}

// /__dev/reload, read as events arrive.
struct EventStreamClient {
    Fd fd;
    std::string received;

    explicit EventStreamClient(uint16_t port) : fd(connectTo(port)) {
        std::string request = "GET /__dev/reload HTTP/1.1\r\nHost: localhost\r\n\r\n";
        REQUIRE(::send(fd.get(), request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
    }

    // Reads until `part` has arrived (after `from`), or the timeout passes.
    bool waitFor(const std::string& part, std::chrono::milliseconds timeout, size_t from = 0) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (received.find(part, from) == std::string::npos) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            pollfd p{fd.get(), POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) continue;
            char buffer[4096];
            ssize_t n = ::recv(fd.get(), buffer, sizeof buffer, 0);
            if (n <= 0) return false;
            received.append(buffer, static_cast<size_t>(n));
        }
        return true;
    }
};

std::string get(uint16_t port, const std::string& target) {
    return fetch(port, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

// A copy of an app's code/ in `dir`, so the server's state (data/,
// private/state/) goes there, not into the source tree.
std::string copyApp(const test::TempDir& dir, const std::string& from) {
    std::filesystem::create_directories(dir.at("app"));
    std::filesystem::copy(from + "/code", dir.at("app/code"), std::filesystem::copy_options::recursive);
    return dir.at("app");
}

// The text in `s` between `before` and the next `after`; empty if absent.
std::string between(const std::string& s, const std::string& before, const std::string& after) {
    size_t at = s.find(before);
    if (at == std::string::npos) return "";
    at += before.size();
    return s.substr(at, s.find(after, at) - at);
}

// The headers a browser would send with a form from the page at `path`:
// the CSRF cookie the page set, and the token in it (from a form's _csrf
// field, or <body>'s hx-headers).
std::string csrfHeaders(uint16_t port, const std::string& path) {
    std::string page = get(port, path);
    std::string cookie = between(page, "Set-Cookie: ff_csrf=", ";");
    std::string token = between(page, "name=\"_csrf\" value=\"", "\"");
    if (token.empty()) token = between(page, "X-CSRF-Token&quot;: &quot;", "&quot;");
    REQUIRE_FALSE(cookie.empty());
    REQUIRE_FALSE(token.empty());
    return "Cookie: ff_csrf=" + cookie + "\r\nX-CSRF-Token: " + token + "\r\n";
}

}  // namespace

TEST_CASE("AppServer: serves an app with code, end to end") {
    std::ostringstream out, err;
    test::TempDir dir;
    auto server = AppServer::startDev(copyApp(dir, FOURFOLD_TEST_APPS_DIR "/scripts"), 0, out, err);
    if (!server && contains(err.str(), "Operation not permitted")) {
        MESSAGE("skipped: binding a local port isn't permitted here");
        return;
    }
    REQUIRE_MESSAGE(server, err.str());
    std::thread serving([&] { server->loop().run(); });
    uint16_t port = server->port();

    std::string home = get(port, "/?name=Ada");
    CHECK(contains(home, "HTTP/1.1 200 OK\r\n"));
    CHECK(contains(home, "<h1>Hello, Ada!</h1><p>3 items</p>"));

    std::string escaped = get(port, "/?name=%3Cscript%3E");
    CHECK(contains(escaped, "Hello, &lt;script&gt;!"));

    std::string api = get(port, "/api");
    CHECK(contains(api, "Content-Type: application/json\r\n"));
    CHECK(contains(api, "{\"ok\":1}"));

    std::string forged = fetch(port,
                               "POST /guestbook/sign HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                               "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 6\r\n\r\nwho=Al");
    CHECK(contains(forged, "HTTP/1.1 403 Forbidden\r\n"));   // no CSRF token
    std::string sign = fetch(port,
                             "POST /guestbook/sign HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
                                 csrfHeaders(port, "/guestbook") +
                                 "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 6\r\n\r\nwho=Al");
    CHECK(contains(sign, "HTTP/1.1 303 See Other\r\n"));
    CHECK(contains(sign, "Location: /guestbook\r\n"));

    CHECK(contains(get(port, "/docs/more"), "more docs"));
    CHECK(contains(get(port, "/style.css"), "margin: 0"));
    CHECK(contains(get(port, "/nope"), "HTTP/1.1 404 Not Found\r\n"));

    server->loop().stop();
    serving.join();
    CHECK(contains(out.str(), "GET /?name=Ada 200"));
}

namespace {

// A server on a scratch copy of an app, running on its own thread.
struct LiveApp {
    fourfold::test::TempDir dir;
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;
    bool skipped = false;

    void start() {
        server = AppServer::startDev(dir.path(), 0, out, err);
        if (!server && contains(err.str(), "Operation not permitted")) {
            skipped = true;
            return;
        }
        REQUIRE_MESSAGE(server, err.str());
        serving = std::thread([this] { server->loop().run(); });
    }
    ~LiveApp() {
        if (serving.joinable()) {
            server->loop().stop();
            serving.join();
        }
    }
    void write(const std::string& rel, const std::string& text) { dir.write("code/" + rel, text); }
};

const char* const kHelloMs = "render = function(req)\n  return {\"who\": \"world\"}\nend function\n";

}  // namespace

TEST_CASE("AppServer: edits show up live, and errors show while the old version serves") {
    LiveApp app;
    app.write("pages/index.html", "<html><body><p>Hello, {{who}}!</p></body></html>");
    app.write("pages/index.ms", kHelloMs);
    app.write("pages/style.css", "p { color: red }");
    app.start();
    if (app.skipped) {
        MESSAGE("skipped: binding a local port isn't permitted here");
        return;
    }
    uint16_t port = app.server->port();
    CHECK(app.server->compiled());

    std::string page = get(port, "/");
    CHECK(contains(page, "<p>Hello, world!</p><script src=\"/__dev/client.js\" nonce=\""));
    CHECK(contains(page, "\" data-generation=\"1\" defer></script>"));
    CHECK(contains(get(port, "/__dev/client.js"), "EventSource"));

    EventStreamClient events(port);
    REQUIRE(events.waitFor("event: hello\ndata: {\"diagnostics\":[],\"generation\":1,\"warnings\":[]}", std::chrono::seconds(2)));
    CHECK(contains(events.received, "Content-Type: text/event-stream\r\n"));

    // An edit to the code: a reload event within about two seconds.
    auto t0 = std::chrono::steady_clock::now();
    app.write("pages/index.ms", "render = function(req)\n  return {\"who\": \"again\"}\nend function\n");
    size_t mark = events.received.size();
    REQUIRE(events.waitFor("event: reload\ndata: {\"generation\":2}", std::chrono::seconds(3), mark));
    auto took = std::chrono::steady_clock::now() - t0;
    MESSAGE("edit to reload event: ", std::chrono::duration_cast<std::chrono::milliseconds>(took).count(), " ms");
    CHECK(took < std::chrono::seconds(2));
    CHECK(contains(get(port, "/"), "<p>Hello, again!</p>"));

    // A stylesheet alone: swapped in place.
    mark = events.received.size();
    app.write("pages/style.css", "p { color: blue }");
    REQUIRE(events.waitFor("event: css\ndata: {\"generation\":3,\"urls\":[\"/style.css\"]}", std::chrono::seconds(3), mark));
    CHECK(contains(get(port, "/style.css"), "blue"));

    // A syntax error: the overlay's errors, and the old version still serves.
    mark = events.received.size();
    app.write("pages/index.ms", "render = function(req)\n  return {\"who\": \nend function\n");
    REQUIRE(events.waitFor("event: errors\n", std::chrono::seconds(3), mark));
    CHECK(contains(events.received.substr(mark), "\"file\":\"code/pages/index.ms\""));
    CHECK(contains(events.received.substr(mark), "\"code\":["));
    page = get(port, "/");
    CHECK(contains(page, "HTTP/1.1 200 OK"));
    CHECK(contains(page, "<p>Hello, again!</p>"));
    CHECK(contains(page, "data-generation=\"3\""));

    // A page opened now hears about the errors at once.
    EventStreamClient late(port);
    REQUIRE(late.waitFor("event: hello\ndata: {\"diagnostics\":[{", std::chrono::seconds(2)));

    // Fixed: reload, to the new version.
    mark = events.received.size();
    app.write("pages/index.ms", kHelloMs);
    REQUIRE(events.waitFor("event: reload\ndata: {\"generation\":4}", std::chrono::seconds(3), mark));
    CHECK(contains(get(port, "/"), "<p>Hello, world!</p>"));

    std::string out = app.out.str();
    CHECK(contains(out, "Recompiled (pages/index.ms)"));
    CHECK(contains(out, "swapping stylesheets"));
    CHECK_FALSE(contains(out, "/__dev/"));   // the dev tools' own requests aren't logged
    CHECK(contains(app.err.str(), "code/pages/index.ms:3: error:"));
    CHECK(contains(app.err.str(), "still serving the last version"));
}

TEST_CASE("AppServer: script errors get the dev error page") {
    LiveApp app;
    app.write("pages/index.ms", "render = function(req)\n  x = 1\n  return nosuch\nend function\n");
    app.start();
    if (app.skipped) return;
    std::string page = get(app.server->port(), "/");
    CHECK(contains(page, "HTTP/1.1 500 "));
    CHECK(contains(page, "Content-Type: text/html; charset=utf-8"));
    CHECK(contains(page, "code/pages/index.ms, line 3"));
    CHECK(contains(page, "<span class=\"hit\"><i>3</i>  return nosuch</span>"));
    CHECK(contains(page, "/__dev/client.js"));
}

TEST_CASE("AppServer: an app that doesn't compile starts anyway, and serves once fixed") {
    LiveApp app;
    app.write("pages/index.html", "<!DOCTYPE html><p>{{who}}</p>");
    app.write("pages/index.ms", "render = function(req)\n  return (\nend function\n");
    app.start();
    if (app.skipped) return;
    CHECK_FALSE(app.server->compiled());
    CHECK(contains(app.err.str(), "code/pages/index.ms:"));
    CHECK(contains(app.err.str(), "the app didn't compile; serving an error page until it does"));
    uint16_t port = app.server->port();

    std::string page = get(port, "/");
    CHECK(contains(page, "HTTP/1.1 503 "));
    CHECK(contains(page, "data-generation=\"0\""));
    EventStreamClient events(port);
    REQUIRE(events.waitFor("event: hello\ndata: {\"diagnostics\":[{", std::chrono::seconds(2)));

    size_t mark = events.received.size();
    app.write("pages/index.ms", kHelloMs);
    REQUIRE(events.waitFor("event: reload\ndata: {\"generation\":1}", std::chrono::seconds(3), mark));
    CHECK(contains(get(port, "/"), "<p>world</p>"));
}

TEST_CASE("AppServer: the guestbook example, with HTMX") {
    std::ostringstream out, err;
    test::TempDir dir;
    auto server = AppServer::startDev(copyApp(dir, FOURFOLD_TESTS_DIR "/../examples/guestbook"), 0, out, err);
    if (!server && contains(err.str(), "Operation not permitted")) {
        MESSAGE("skipped: binding a local port isn't permitted here");
        return;
    }
    REQUIRE_MESSAGE(server, err.str());
    REQUIRE_MESSAGE(server->compiled(), err.str());
    std::thread serving([&] { server->loop().run(); });
    uint16_t port = server->port();

    std::string page = get(port, "/");
    CHECK(contains(page, "HTTP/1.1 200 OK\r\n"));
    // The CSP's nonce is on every <script> in the page.
    size_t at = page.find("'nonce-");
    REQUIRE(at != std::string::npos);
    std::string nonce = page.substr(at + 7, page.find('\'', at + 7) - at - 7);
    size_t scripts = 0, nonced = 0;
    for (size_t s = page.find("<script"); s != std::string::npos; s = page.find("<script", s + 1)) {
        scripts++;
        size_t end = page.find('>', s);
        if (page.substr(s, end - s).find("nonce=\"" + nonce + "\"") != std::string::npos) nonced++;
    }
    CHECK(scripts >= 4);
    CHECK_EQ(nonced, scripts);
    // Hostile sample entries stay text.
    CHECK(contains(page, "<b>&lt;script&gt;alert(&#39;hi&#39;)&lt;/script&gt;</b>"));
    CHECK_FALSE(contains(page, "javascript:"));
    CHECK_FALSE(contains(page, "<img"));
    CHECK(contains(page, "<link rel=\"stylesheet\" href=\"/__fourfold/markdown.css\">"));
    CHECK(contains(page, "{\"entries\":4,\"names\":4}"));

    // Signing with HTMX gets just the new entry, escaped.
    std::string body = "who=%3Cimg+src%3Dx+onerror%3Dalert(1)%3E&msg=**hi**";
    std::string csrf = csrfHeaders(port, "/");
    std::string signed_ = fetch(port, "POST /sign HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nHX-Request: true\r\n" +
                                      csrf + "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                                          std::to_string(body.size()) + "\r\n\r\n" + body);
    CHECK(contains(signed_, "HTTP/1.1 200 OK\r\n"));
    CHECK(contains(signed_, "<li><b>&lt;img src=x onerror=alert(1)&gt;</b>: <div class=\"markdown comment\">\n"
                            "<p><strong>hi</strong></p>\n</div>\n</li>"));
    CHECK_FALSE(contains(signed_, "/__dev/client.js"));   // a fragment, not a page
    // Without HTMX, a redirect back.
    std::string plain = fetch(port, "POST /sign HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + csrf +
                                    "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 11\r\n\r\nwho=a&msg=b");
    CHECK(contains(plain, "HTTP/1.1 303 See Other\r\n"));
    CHECK(contains(plain, "Location: /\r\n"));

    CHECK(contains(get(port, "/about"), "<h1 id=\"about-this-guestbook\">"));
    std::string htmx = get(port, "/__fourfold/htmx.js");
    CHECK(contains(htmx, "htmx.config.allowEval = false;"));

    server->loop().stop();
    serving.join();
}

// End to end: `fourfold dev`'s server, on a real socket, serving
// tests/apps/scripts with its code running on the script thread.
#include "dev_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::DevServer;

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

std::string get(uint16_t port, const std::string& target) {
    return fetch(port, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("DevServer: serves an app with code, end to end") {
    std::ostringstream out, err;
    auto server = DevServer::start(FOURFOLD_TEST_APPS_DIR "/scripts", 0, out, err);
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

    std::string sign = fetch(port,
                             "POST /guestbook/sign HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
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

TEST_CASE("DevServer: an app that doesn't compile doesn't start") {
    fourfold::test::TempDir dir;
    dir.write("code/pages/index.ms", "render = function(req)\n  return (\nend function\n");
    std::ostringstream out, err;
    auto server = DevServer::start(dir.path(), 0, out, err);
    CHECK_FALSE(server);
    CHECK(contains(err.str(), "code/pages/index.ms:"));
    CHECK(contains(err.str(), "fourfold dev: the app didn't compile."));
}

#include "httpserver/http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

#include "doctest.h"

using namespace fourfold;
using std::chrono::milliseconds;

namespace {

// Number of complete responses at the start of `data` (skipping any
// "100 Continue"), assuming each has a Content-Length or no body.
size_t countResponses(const std::string& data) {
    size_t count = 0, pos = 0;
    for (;;) {
        size_t end = data.find("\r\n\r\n", pos);
        if (end == std::string::npos) return count;
        std::string head = data.substr(pos, end - pos);
        size_t bodyLength = 0;
        size_t cl = head.find("Content-Length: ");
        if (cl != std::string::npos) bodyLength = std::stoul(head.substr(cl + 16));
        if (head.find(" HEAD-REQUEST") != std::string::npos) bodyLength = 0;
        size_t next = end + 4 + bodyLength;
        if (next > data.size()) return count;
        if (head.compare(0, 12, "HTTP/1.1 100") != 0) count++;
        pos = next;
    }
}

// An HttpServer on a socket pair, with the test holding the client end.
struct Harness {
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<HttpServer> server;
    Fd client;
    std::string received;      // everything the client has read
    bool clientSawEof = false;
    std::vector<HttpRequest> requests;

    explicit Harness(HttpServerLimits limits = {}, RequestHandler handler = nullptr) {
        auto created = EventLoop::create();
        REQUIRE(created.ok());
        loop = std::move(created).value();
        if (!handler) {
            // Echo: "<method> <target> <body>"
            handler = [this](HttpRequest request, Responder respond) {
                std::string echo = request.method + " " + request.target + " " + request.body;
                requests.push_back(std::move(request));
                respond(HttpResponse::text(200, echo));
            };
        }
        server = std::make_unique<HttpServer>(*loop, std::move(handler), limits);
        auto pair = makeSocketPair();
        REQUIRE(pair.ok());
        client = std::move(pair.value().first);
        server->addConnection(std::move(pair.value().second), "test-peer");
    }

    // Runs the loop briefly and collects what the client can read.
    void pump(milliseconds wait = milliseconds(5)) {
        loop->runOnce(wait);
        char buffer[16384];
        for (;;) {
            auto n = readSome(client.get(), buffer, sizeof buffer);
            if (!n) break;
            if (n.value() == 0) {
                clientSawEof = true;
                break;
            }
            received.append(buffer, n.value());
        }
    }

    // Sends all of `data`, pumping the loop whenever the socket is full.
    void send(const std::string& data) {
        size_t sent = 0;
        for (int i = 0; sent < data.size() && i < 10000; i++) {
            auto n = writeSome(client.get(), data.data() + sent, data.size() - sent);
            if (n) {
                sent += n.value();
            } else if (n.error().code == ErrorCode::WouldBlock) {
                pump(milliseconds(1));
            } else {
                break;   // the server closed on us (e.g. after a 413)
            }
        }
    }

    // Pumps until `count` responses have arrived, or the client sees EOF.
    void waitForResponses(size_t count) {
        for (int i = 0; i < 400 && countResponses(received) < count && !clientSawEof; i++) pump();
    }

    void waitForEof() {
        for (int i = 0; i < 400 && !clientSawEof; i++) pump();
    }
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("HttpServer: answers a GET") {
    Harness h;
    h.send("GET /hello?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    h.waitForResponses(1);
    CHECK_EQ(h.received.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    CHECK(contains(h.received, "Content-Length: 15\r\n"));
    CHECK(contains(h.received, "\r\n\r\nGET /hello?x=1 "));
    REQUIRE_EQ(h.requests.size(), 1u);
    const HttpRequest& r = h.requests[0];
    CHECK_EQ(r.method, "GET");
    CHECK_EQ(r.target, "/hello?x=1");
    CHECK_EQ(r.versionMinor, 1);
    CHECK_EQ(*r.headers.get("host"), "example.com");
    CHECK_EQ(r.peerAddress, "test-peer");
    CHECK_FALSE(h.clientSawEof);   // keep-alive
}

TEST_CASE("HttpServer: keeps the connection for further requests") {
    Harness h;
    h.send("GET /one HTTP/1.1\r\nHost: a\r\n\r\n");
    h.waitForResponses(1);
    h.send("GET /two HTTP/1.1\r\nHost: a\r\n\r\n");
    h.waitForResponses(2);
    CHECK_EQ(countResponses(h.received), 2u);
    CHECK_EQ(h.server->connectionCount(), 1u);
    CHECK_FALSE(h.clientSawEof);
}

TEST_CASE("HttpServer: tells an HTTP/1.0 client when it keeps the connection") {
    Harness h;
    h.send("GET /one HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    h.waitForResponses(1);
    CHECK(h.received.find("Connection: keep-alive\r\n") != std::string::npos);
    h.send("GET /two HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    h.waitForResponses(2);
    CHECK_EQ(countResponses(h.received), 2u);
    CHECK_FALSE(h.clientSawEof);

    Harness plain;
    plain.send("GET /one HTTP/1.0\r\n\r\n");
    plain.waitForResponses(1);
    CHECK(plain.received.find("Connection: close\r\n") != std::string::npos);
    CHECK(plain.received.find("keep-alive") == std::string::npos);
}

TEST_CASE("HttpServer: answers pipelined requests in order") {
    Harness h;
    h.send("GET /a HTTP/1.1\r\nHost: a\r\n\r\n"
           "GET /b HTTP/1.1\r\nHost: a\r\n\r\n"
           "GET /c HTTP/1.1\r\nHost: a\r\n\r\n");
    h.waitForResponses(3);
    REQUIRE_EQ(countResponses(h.received), 3u);
    size_t a = h.received.find("GET /a"), b = h.received.find("GET /b"), c = h.received.find("GET /c");
    CHECK(a < b);
    CHECK(b < c);
}

TEST_CASE("HttpServer: reads a request body, by length or chunked") {
    Harness h;
    h.send("POST /form HTTP/1.1\r\nHost: a\r\nContent-Length: 7\r\n\r\nname=Jo");
    h.send("POST /chunks HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
           "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    h.waitForResponses(2);
    REQUIRE_EQ(h.requests.size(), 2u);
    CHECK_EQ(h.requests[0].body, "name=Jo");
    CHECK_EQ(h.requests[1].body, "abcde");
}

TEST_CASE("HttpServer: a request arriving a byte at a time") {
    Harness h;
    std::string request = "GET /slow HTTP/1.1\r\nHost: a\r\n\r\n";
    for (char ch : request) {
        h.send(std::string(1, ch));
        h.pump(milliseconds(0));
    }
    h.waitForResponses(1);
    REQUIRE_EQ(h.requests.size(), 1u);
    CHECK_EQ(h.requests[0].target, "/slow");
}

TEST_CASE("HttpServer: HEAD gets headers but no body") {
    Harness h;
    h.send("HEAD /page HTTP/1.1\r\nHost: a\r\n\r\nGET /next HTTP/1.1\r\nHost: a\r\n\r\n");
    // The HEAD response has a Content-Length but no body, so read until the
    // GET's response shows up instead of counting.
    for (int i = 0; i < 400 && !contains(h.received, "GET /next"); i++) h.pump();
    CHECK_EQ(h.received.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    CHECK_FALSE(contains(h.received, "HEAD /page"));
    CHECK(contains(h.received, "\r\n\r\nHTTP/1.1 200 OK\r\n"));   // HEAD response ends at its headers
}

TEST_CASE("HttpServer: closes after a response when asked") {
    SUBCASE("Connection: close from the client") {
        Harness h;
        h.send("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
        h.waitForEof();
        CHECK_EQ(countResponses(h.received), 1u);
        CHECK(contains(h.received, "Connection: close\r\n"));
    }
    SUBCASE("HTTP/1.0 without keep-alive") {
        Harness h;
        h.send("GET / HTTP/1.0\r\n\r\n");
        h.waitForEof();
        CHECK_EQ(countResponses(h.received), 1u);
    }
    SUBCASE("Connection: close from the handler") {
        Harness h({}, [](HttpRequest, Responder respond) {
            HttpResponse r = HttpResponse::text(200, "bye");
            r.headers.set("Connection", "close");
            respond(r);
        });
        h.send("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
        h.waitForEof();
        CHECK_EQ(countResponses(h.received), 1u);
    }
    SUBCASE("the client shuts its side after sending") {
        Harness h;
        h.send("GET /last HTTP/1.1\r\nHost: a\r\n\r\n");
        shutdown(h.client.get(), SHUT_WR);
        h.waitForEof();
        CHECK_EQ(countResponses(h.received), 1u);
        CHECK(contains(h.received, "GET /last"));
    }
}

TEST_CASE("HttpServer: rejects bad requests, then closes") {
    HttpServerLimits limits;
    limits.maxHeaderBytes = 1024;
    limits.maxHeaderCount = 10;
    limits.maxBodyBytes = 100;

    struct Case {
        const char* name;
        std::string request;
        const char* status;
    };
    std::vector<Case> cases = {
        {"malformed request line", "GARBAGE\r\n\r\n", "HTTP/1.1 400 "},
        {"bad header", "GET / HTTP/1.1\r\nNo colon here\r\n\r\n", "HTTP/1.1 400 "},
        {"URL too long", "GET /" + std::string(2000, 'a') + " HTTP/1.1\r\n\r\n", "HTTP/1.1 414 "},
        {"headers too large", "GET / HTTP/1.1\r\nX-Big: " + std::string(2000, 'b') + "\r\n\r\n",
         "HTTP/1.1 431 "},
        {"too many headers",
         "GET / HTTP/1.1\r\n" + [] {
             std::string s;
             for (int i = 0; i < 11; i++) s += "X-H" + std::to_string(i) + ": v\r\n";
             return s;
         }() + "\r\n",
         "HTTP/1.1 431 "},
        {"declared body too large", "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 101\r\n\r\n",
         "HTTP/1.1 413 "},
        {"chunked body too large",
         "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n64\r\n" +
             std::string(100, 'c') + "\r\n1\r\nc\r\n0\r\n\r\n",
         "HTTP/1.1 413 "},
        {"both length and chunked (request smuggling)",
         "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
         "HTTP/1.1 400 "},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Harness h(limits);
        h.send(c.request);
        h.waitForEof();
        CHECK_EQ(h.received.substr(0, 13), c.status);
        CHECK(contains(h.received, "Connection: close\r\n"));
        CHECK(h.clientSawEof);
        CHECK(h.requests.empty());
    }
}

TEST_CASE("HttpServer: the handler can respond later") {
    Responder saved;
    Harness h({}, [&](HttpRequest, Responder respond) { saved = std::move(respond); });
    h.send("GET /wait HTTP/1.1\r\nHost: a\r\n\r\n");
    for (int i = 0; i < 20; i++) h.pump();
    CHECK(h.received.empty());
    REQUIRE(saved);
    saved(HttpResponse::text(202, "later"));
    saved(HttpResponse::text(500, "twice"));   // a second answer is ignored
    h.waitForResponses(1);
    CHECK_EQ(h.received.rfind("HTTP/1.1 202 Unknown\r\n", 0), 0u);
    CHECK_FALSE(contains(h.received, "twice"));
}

TEST_CASE("HttpServer: a late response to a closed connection is dropped") {
    Responder saved;
    Harness h({}, [&](HttpRequest, Responder respond) { saved = std::move(respond); });
    h.send("GET /wait HTTP/1.1\r\nHost: a\r\n\r\n");
    for (int i = 0; i < 5; i++) h.pump();
    REQUIRE(saved);
    h.server.reset();   // closes all connections
    saved(HttpResponse::text(200, "too late"));   // must not crash
    h.pump();
    CHECK(h.clientSawEof);
}

TEST_CASE("HttpServer: timeouts") {
    HttpServerLimits limits;
    limits.requestTimeout = milliseconds(30);
    limits.idleTimeout = milliseconds(30);

    SUBCASE("an incomplete request gets 408") {
        Harness h(limits);
        h.send("GET /partial HTTP/1.1\r\nHost: a\r\n");
        h.waitForEof();
        CHECK_EQ(h.received.substr(0, 13), "HTTP/1.1 408 ");
    }
    SUBCASE("an idle connection is closed without a response") {
        Harness h(limits);
        h.waitForEof();
        CHECK(h.clientSawEof);
        CHECK(h.received.empty());
        CHECK_EQ(h.server->connectionCount(), 0u);
    }
}

namespace {

// A handler that answers with a stream, keeping the writer for the test.
RequestHandler streamingHandler(std::shared_ptr<StreamWriter>& writer) {
    return [&writer](HttpRequest, Responder respond) {
        HttpResponse r;
        r.headers.set("Content-Type", "text/event-stream");
        r.stream = [&writer](std::shared_ptr<StreamWriter> w) {
            w->write("event: hello\n\n");
            writer = std::move(w);
        };
        respond(std::move(r));
    };
}

}  // namespace

TEST_CASE("HttpServer: streams a response body") {
    std::shared_ptr<StreamWriter> writer;
    HttpServerLimits limits;
    limits.requestTimeout = milliseconds(30);
    limits.idleTimeout = milliseconds(30);
    Harness h(limits, streamingHandler(writer));
    h.send("GET /events HTTP/1.1\r\nHost: a\r\n\r\n");
    for (int i = 0; i < 20 && !contains(h.received, "hello"); i++) h.pump();
    REQUIRE(writer);
    CHECK(writer->isOpen());
    CHECK_EQ(h.received.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    CHECK_FALSE(contains(h.received, "Content-Length"));
    CHECK(contains(h.received, "Connection: close\r\n"));
    CHECK(contains(h.received, "\r\n\r\nevent: hello\n\n"));

    SUBCASE("timeouts don't apply, and later writes arrive") {
        for (int i = 0; i < 20; i++) h.pump(milliseconds(5));   // well past both timeouts
        CHECK(writer->isOpen());
        CHECK(writer->write("data: 1\n\n"));
        h.pump();
        CHECK(contains(h.received, "data: 1\n\n"));
        CHECK_FALSE(h.clientSawEof);
    }
    SUBCASE("closing the stream ends the connection") {
        writer->write("data: last\n\n");
        writer->close();
        CHECK_FALSE(writer->isOpen());
        CHECK_FALSE(writer->write("dropped"));
        h.waitForEof();
        CHECK(h.clientSawEof);
        CHECK(contains(h.received, "data: last\n\n"));
        CHECK_FALSE(contains(h.received, "dropped"));
    }
    SUBCASE("the client leaving closes the stream") {
        h.client.reset();
        for (int i = 0; i < 20 && writer->isOpen(); i++) h.loop->runOnce(milliseconds(5));
        CHECK_FALSE(writer->isOpen());
        CHECK_FALSE(writer->write("data: x\n\n"));
        CHECK_EQ(h.server->connectionCount(), 0u);
    }
    SUBCASE("a client that doesn't read is dropped") {
        std::string chunk(64 * 1024, 'x');
        bool ok = true;
        for (int i = 0; i < 100 && ok; i++) ok = writer->write(chunk);   // 6.4 MB, past the 1 MB cap
        CHECK_FALSE(ok);
        CHECK_FALSE(writer->isOpen());
    }
}

TEST_CASE("HttpServer: HEAD of a streamed response gets headers only") {
    std::shared_ptr<StreamWriter> writer;
    Harness h({}, streamingHandler(writer));
    h.send("HEAD /events HTTP/1.1\r\nHost: a\r\n\r\n");
    h.waitForResponses(1);
    CHECK_FALSE(writer);
    CHECK_EQ(h.received.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    CHECK_FALSE(contains(h.received, "hello"));
}

TEST_CASE("HttpServer: framing headers are the server's, and line breaks are dropped") {
    Harness h({}, [](HttpRequest, Responder respond) {
        HttpResponse r = HttpResponse::text(200, "body");
        r.headers.add("Content-Length", "999");
        r.headers.add("X-Evil", "a\r\nSet-Cookie: injected=1");
        r.headers.add("X-Fine", "ok");
        respond(r);
    });
    h.send("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    h.waitForResponses(1);
    CHECK(contains(h.received, "Content-Length: 4\r\n"));
    CHECK_FALSE(contains(h.received, "999"));
    CHECK_FALSE(contains(h.received, "injected"));
    CHECK(contains(h.received, "X-Fine: ok\r\n"));
}

TEST_CASE("HttpServer: 204 and 304 have no body or Content-Length") {
    Harness h({}, [](HttpRequest, Responder respond) {
        HttpResponse r;
        r.status = 204;
        r.body = "ignored";
        respond(r);
    });
    h.send("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    for (int i = 0; i < 400 && !contains(h.received, "\r\n\r\n"); i++) h.pump();
    CHECK_EQ(h.received, "HTTP/1.1 204 No Content\r\n\r\n");
}

TEST_CASE("HttpServer: serves real TCP connections") {
    auto created = EventLoop::create();
    REQUIRE(created.ok());
    auto loop = std::move(created).value();
    HttpServer server(*loop, [](HttpRequest request, Responder respond) {
        respond(HttpResponse::text(200, "from " + request.peerAddress));
    });
    Status listening = server.listen("127.0.0.1", 0);
    if (!listening.ok() && listening.error().code == ErrorCode::Denied) {
        MESSAGE("skipped: binding a local port isn't permitted here");
        return;
    }
    REQUIRE(listening.ok());
    REQUIRE(server.port() != 0);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    Fd client(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    // Claude Code's macOS sandbox sometimes refuses a process's first
    // loopback connections (EPERM) for a moment, then allows them. Retry
    // briefly, for EPERM only; any other failure still fails the test.
    for (int tries = 0; connected < 0 && errno == EPERM && tries < 5; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    }
    std::string connectError = connected == 0 ? "ok" : std::strerror(errno);
    INFO("connect to port " << server.port() << ": " << connectError);
    REQUIRE(connected == 0);
    REQUIRE(setNonBlocking(fd).ok());
    std::string request = "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n";
    REQUIRE(writeSome(fd, request.data(), request.size()).ok());

    std::string received;
    for (int i = 0; i < 400; i++) {
        loop->runOnce(milliseconds(5));
        char buffer[4096];
        auto n = readSome(fd, buffer, sizeof buffer);
        if (n && n.value() == 0) break;
        if (n) received.append(buffer, n.value());
    }
    CHECK_EQ(received.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    CHECK(contains(received, "from 127.0.0.1"));
}

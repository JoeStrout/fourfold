// fourfold front (M11.1): the request passed on to an engine, Upstream
// against a fake engine on a Unix socket (reuse, failures, streams), and
// FrontServer end to end over TLS.
#include "front/front_server.h"
#include "front/upstream.h"

#include <arpa/inet.h>
#include <csignal>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "doctest.h"
#include "platform/socket.h"
#include "temp_dir.h"
#include "tls/certificates.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A loop on a thread of its own; run() posts work to it and waits.
struct LoopThread {
    std::unique_ptr<EventLoop> loop = EventLoop::create().value();
    std::thread thread{[this] { loop->run(); }};
    ~LoopThread() {
        loop->stop();
        thread.join();
    }
    template <class F>
    void run(F f) {
        std::promise<void> done;
        loop->post([&] {
            f();
            done.set_value();
        });
        done.get_future().wait();
    }
};

// What a fake engine saw, and how it answers.
struct FakeEngine {
    std::vector<HttpRequest> seen;
    std::function<HttpResponse(const HttpRequest&)> answer = [](const HttpRequest& r) {
        return HttpResponse::text(200, "engine: " + r.method + " " + r.target + " " + r.body);
    };
    std::unique_ptr<HttpServer> server;

    void start(EventLoop& loop, const std::string& socket) {
        server = std::make_unique<HttpServer>(loop, [this](HttpRequest r, Responder respond) {
            seen.push_back(r);
            respond(answer(r));
        });
        REQUIRE(server->listenUnix(socket));
    }
};

// Sends one request through `up`, and waits for the response.
HttpResponse through(LoopThread& t, Upstream& up, HttpRequest r, const std::string& visitor = "203.0.113.5") {
    std::promise<HttpResponse> got;
    t.loop->post([&] {
        up.forward(std::move(r), visitor, "https", [&got](HttpResponse response) { got.set_value(std::move(response)); });
    });
    auto f = got.get_future();
    REQUIRE(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    return f.get();
}

HttpRequest get(const std::string& target, const std::string& host = "club.example.com") {
    HttpRequest r;
    r.method = "GET";
    r.target = target;
    r.headers.add("Host", host);
    return r;
}

// A StreamWriter that records what it's given.
struct Recorder : StreamWriter {
    std::mutex m;
    std::string got;
    bool open = true;
    bool write(std::string bytes) override {
        std::lock_guard<std::mutex> lock(m);
        got += bytes;
        return open;
    }
    void close() override {
        std::lock_guard<std::mutex> lock(m);
        open = false;
    }
    bool isOpen() const override { return open; }
    std::string text() {
        std::lock_guard<std::mutex> lock(m);
        return got;
    }
};

}  // namespace

TEST_CASE("upstreamRequest: hop-by-hop and forwarding fields dropped, Forwarded added, HEAD as GET") {
    HttpRequest r;
    r.method = "POST";
    r.target = "/sign?x=1";
    r.headers.add("Host", "club.example.com");
    r.headers.add("Connection", "keep-alive, X-Secret");
    r.headers.add("X-Secret", "hop");
    r.headers.add("Keep-Alive", "timeout=5");
    r.headers.add("Forwarded", "for=6.6.6.6");
    r.headers.add("X-Forwarded-For", "6.6.6.6");
    r.headers.add("X-Real-IP", "6.6.6.6");
    r.headers.add("Expect", "100-continue");
    r.headers.add("Content-Length", "999");
    r.headers.add("Transfer-Encoding", "chunked");
    r.headers.add("Cookie", "a=b");
    r.body = "name=Joe";
    std::string wire = upstreamRequest(r, "198.51.100.7", "https");
    CHECK(wire.compare(0, 31, "POST /sign?x=1 HTTP/1.1\r\nHost: ") == 0);
    CHECK(contains(wire, "\r\nCookie: a=b\r\n"));
    CHECK(contains(wire, "\r\nForwarded: for=198.51.100.7;proto=https;host=club.example.com\r\n"));
    CHECK(contains(wire, "\r\nContent-Length: 8\r\n\r\nname=Joe"));
    for (const char* gone : {"6.6.6.6", "X-Secret", "Keep-Alive", "Expect", "999", "chunked", "Connection:"}) {
        CHECK_MESSAGE(!contains(wire, gone), gone);
    }

    HttpRequest head = get("/");
    head.method = "HEAD";
    CHECK(upstreamRequest(head, "::1", "https").compare(0, 15, "GET / HTTP/1.1\r") == 0);
    CHECK_FALSE(contains(upstreamRequest(head, "::1", "https"), "Content-Length"));

    CHECK_EQ(forwardedElement("2001:db8::7", "https", "a.com:8443"), "for=\"[2001:db8::7]\";proto=https;host=\"a.com:8443\"");
    CHECK_EQ(forwardedElement("::ffff:198.51.100.7", "https", ""), "for=198.51.100.7;proto=https");
    CHECK_EQ(forwardedElement("1.2.3.4", "https", "bad\"host"), "for=1.2.3.4;proto=https");
}

TEST_CASE("Upstream: requests reach the engine, connections are reused, and failures are front's own") {
    TempDir dir;
    LoopThread t;
    FakeEngine engine;
    std::string socket = dir.at("e.sock");
    t.run([&] { engine.start(*t.loop, socket); });
    auto up = Upstream::create(*t.loop, "club", socket);

    HttpRequest post = get("/sign");
    post.method = "POST";
    post.body = "hello";
    HttpResponse r = through(t, *up, post);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "engine: POST /sign hello");
    REQUIRE_EQ(engine.seen.size(), 1u);
    CHECK_EQ(engine.seen[0].peerAddress, std::string(kUnixPeer));
    CHECK_EQ(*engine.seen[0].headers.get("Forwarded"), "for=203.0.113.5;proto=https;host=club.example.com");

    // Kept, and used again.
    r = through(t, *up, get("/two"));
    CHECK_EQ(r.body, "engine: GET /two ");
    t.run([&] {
        CHECK_EQ(up->idleCount(), 1u);
        CHECK_EQ(engine.server->connectionCount(), 1u);
    });

    // Headers the engine sends are passed back; hop-by-hop ones aren't.
    engine.answer = [](const HttpRequest&) {
        HttpResponse x = HttpResponse::text(404, "nope");
        x.headers.set("Set-Cookie", "s=1; Secure");
        x.headers.set("X-Custom", "yes");
        return x;
    };
    r = through(t, *up, get("/missing"));
    CHECK_EQ(r.status, 404);
    CHECK_EQ(*r.headers.get("Set-Cookie"), "s=1; Secure");
    CHECK_FALSE(r.headers.has("Content-Length"));
    CHECK_FALSE(r.headers.has("Connection"));

    // The engine restarted: the kept connection is dead, and a fresh one is
    // tried, once.
    t.run([&] { engine.server.reset(); });
    t.run([&] { engine.start(*t.loop, socket); });
    engine.answer = [](const HttpRequest&) { return HttpResponse::text(200, "back"); };
    r = through(t, *up, get("/again"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "back");

    // Not running at all: a 502.
    t.run([&] { engine.server.reset(); });
    ::unlink(socket.c_str());
    r = through(t, *up, get("/"));
    CHECK_EQ(r.status, 502);
    CHECK(contains(r.body, "isn't running"));
    t.run([&] { up->closeAll(); });
}

TEST_CASE("Upstream: a too-slow engine is a 504; a malformed answer a 502") {
    TempDir dir;
    LoopThread t;
    std::string socket = dir.at("e.sock");
    std::vector<Responder> held;
    std::unique_ptr<HttpServer> slow;
    t.run([&] {
        slow = std::make_unique<HttpServer>(*t.loop, [&held](HttpRequest, Responder respond) { held.push_back(respond); });
        REQUIRE(slow->listenUnix(socket));
    });
    UpstreamLimits limits;
    limits.responseTimeout = std::chrono::milliseconds(200);
    auto up = Upstream::create(*t.loop, "club", socket, limits);
    HttpResponse r = through(t, *up, get("/"));
    CHECK_EQ(r.status, 504);
    t.run([&] {
        held.clear();
        slow.reset();
    });

    // Garbage instead of HTTP.
    auto listening = listenUnix(socket);
    REQUIRE(listening.ok());
    int raw = listening.value().get();
    std::thread garbage([raw] {
        for (int i = 0; i < 200; i++) {
            auto a = acceptConnection(raw);
            if (a.ok() && a.value()) {
                char buffer[4096];
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                (void)readSome(a.value()->fd.get(), buffer, sizeof buffer);
                (void)writeSome(a.value()->fd.get(), "NOT HTTP AT ALL\r\n\r\n", 19);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    r = through(t, *up, get("/"));
    garbage.join();
    CHECK_EQ(r.status, 502);
    t.run([&] { up->closeAll(); });
}

TEST_CASE("Upstream: a streamed response (server-sent events) is passed on as it comes") {
    TempDir dir;
    LoopThread t;
    FakeEngine engine;
    std::string socket = dir.at("e.sock");
    std::shared_ptr<StreamWriter> engineSide;
    engine.answer = [&engineSide](const HttpRequest&) {
        HttpResponse r;
        r.headers.set("Content-Type", "text/event-stream");
        r.stream = [&engineSide](std::shared_ptr<StreamWriter> w) {
            w->write("data: first\n\n");
            engineSide = std::move(w);
        };
        return r;
    };
    t.run([&] { engine.start(*t.loop, socket); });
    auto up = Upstream::create(*t.loop, "club", socket);

    HttpResponse r = through(t, *up, get("/live"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(*r.headers.get("Content-Type"), "text/event-stream");
    REQUIRE(r.stream);
    auto visitor = std::make_shared<Recorder>();
    t.run([&] { r.stream(visitor); });
    for (int i = 0; i < 100 && !contains(visitor->text(), "first"); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(visitor->text(), "data: first\n\n");
    t.run([&] { engineSide->write("data: second\n\n"); });
    for (int i = 0; i < 100 && !contains(visitor->text(), "second"); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(visitor->text(), "data: first\n\ndata: second\n\n");

    // The engine ends it: so does the visitor's.
    t.run([&] { engineSide->close(); });
    for (int i = 0; i < 100 && visitor->isOpen(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(visitor->isOpen());
    t.run([&] { CHECK_EQ(up->connectionCount(), 0u); });

    // The visitor goes: the engine's end is closed when the next bytes come.
    r = through(t, *up, get("/live"));
    auto gone = std::make_shared<Recorder>();
    t.run([&] { r.stream(gone); });
    gone->close();
    t.run([&] { engineSide->write("data: anyone?\n\n"); });
    for (int i = 0; i < 100; i++) {
        bool closed = false;
        t.run([&] { closed = up->connectionCount() == 0; });
        if (closed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    t.run([&] { CHECK_EQ(up->connectionCount(), 0u); });
    t.run([&] {
        up->closeAll();
        engineSide.reset();
        engine.server.reset();
    });
}

namespace {

// One HTTPS request over TLS to 127.0.0.1:port, with `sni`; the whole
// response (Connection: close), or "" if TLS failed.
std::string httpsFetch(uint16_t port, const std::string& request, const std::string& sni) {
    std::signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return "";
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, sni.c_str());
    std::string response;
    if (SSL_connect(ssl) == 1) {
        X509* cert = SSL_get1_peer_certificate(ssl);
        if (cert) {
            char name[256] = {0};
            X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, name, sizeof name);
            response = std::string("[cert ") + name + "]";
            X509_free(cert);
        }
        SSL_write(ssl, request.data(), static_cast<int>(request.size()));
        char buffer[4096];
        int n;
        while ((n = SSL_read(ssl, buffer, sizeof buffer)) > 0) response.append(buffer, static_cast<size_t>(n));
    }
    ERR_clear_error();
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return response;
}

std::string plainFetch(uint16_t port, const std::string& request) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return "";
    }
    (void)!::write(fd, request.data(), request.size());
    std::string out;
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof buffer)) > 0) out.append(buffer, static_cast<size_t>(n));
    ::close(fd);
    return out;
}

}  // namespace

TEST_CASE("FrontServer: each app by its name, with its own certificate; port 80 redirects") {
    TempDir dir;
    LoopThread t;
    FakeEngine club, shop;
    club.answer = [](const HttpRequest& r) { return HttpResponse::text(200, "club " + r.target); };
    shop.answer = [](const HttpRequest& r) { return HttpResponse::text(200, "shop " + r.target); };
    t.run([&] {
        club.start(*t.loop, dir.at("club.sock"));
        shop.start(*t.loop, dir.at("shop.sock"));
    });
    FrontOptions o;
    for (const char* name : {"club", "shop"}) {
        std::string domain = std::string(name) + ".example.com";
        auto key = generatePrivateKey().value();
        dir.write(std::string(name) + ".key", key);
        dir.write(std::string(name) + ".crt", selfSignedCertificate({domain}, key, 30).value());
        FrontApp app;
        app.name = name;
        app.domains = {domain};
        app.socket = dir.at(std::string(name) + ".sock");
        app.certFile = dir.at(std::string(name) + ".crt");
        app.keyFile = dir.at(std::string(name) + ".key");
        o.config.apps.push_back(app);
    }
    o.stateFolder = dir.at("state");
    o.address = "127.0.0.1";
    o.port = 0;
    o.redirectAddress = "127.0.0.1";
    o.redirectPort = 0;
    std::unique_ptr<FrontServer> front;
    t.run([&] {
        auto created = FrontServer::create(*t.loop, std::move(o));
        REQUIRE_MESSAGE(created.ok(), created.error().message);
        front = std::move(created).value();
        front->start();
    });
    uint16_t port = front->port();

    std::string a = httpsFetch(port, "GET /hi HTTP/1.1\r\nHost: club.example.com\r\nConnection: close\r\n\r\n",
                               "club.example.com");
    CHECK(contains(a, "[cert club.example.com]"));
    CHECK(contains(a, "200 OK"));
    CHECK(contains(a, "club /hi"));
    std::string b = httpsFetch(port, "GET /x HTTP/1.1\r\nHost: shop.example.com:443\r\nConnection: close\r\n\r\n",
                               "shop.example.com");
    CHECK(contains(b, "[cert shop.example.com]"));
    CHECK(contains(b, "shop /x"));
    // Not a site here.
    std::string c = httpsFetch(port, "GET / HTTP/1.1\r\nHost: evil.example.net\r\nConnection: close\r\n\r\n",
                               "club.example.com");
    CHECK(contains(c, "421 Misdirected Request"));
    // The visitor, as the engine is told.
    t.run([&] {
        REQUIRE_FALSE(club.seen.empty());
        CHECK_EQ(*club.seen.back().headers.get("Forwarded"), "for=127.0.0.1;proto=https;host=club.example.com");
    });

    // Port 80: to HTTPS, for the apps' names only.
    uint16_t http = front->redirectPort();
    std::string moved = plainFetch(http, "GET /a?b=1 HTTP/1.1\r\nHost: shop.example.com\r\nConnection: close\r\n\r\n");
    CHECK(contains(moved, "301 "));
    CHECK(contains(moved, "Location: https://shop.example.com:" + std::to_string(port) + "/a?b=1\r\n"));
    CHECK(contains(plainFetch(http, "GET / HTTP/1.1\r\nHost: evil.example.net\r\nConnection: close\r\n\r\n"), "421 "));

    // Shutdown finishes, and closes the engines' connections.
    std::promise<void> stopped;
    t.loop->post([&] { front->shutdown(std::chrono::seconds(2), [&stopped] { stopped.set_value(); }); });
    REQUIRE(stopped.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    t.run([&] {
        for (auto& up : front->upstreams()) CHECK_EQ(up->connectionCount(), 0u);
        front.reset();
        club.server.reset();
        shop.server.reset();
    });
}

TEST_CASE("FrontServer: an app with no certifiable name and no files is refused at startup") {
    TempDir dir;
    LoopThread t;
    FrontOptions o;
    FrontApp app;
    app.name = "local";
    app.domains = {"localhost"};
    app.socket = dir.at("l.sock");
    o.config.apps.push_back(app);
    o.stateFolder = dir.at("state");
    o.address = "127.0.0.1";
    o.port = 0;
    o.redirectAddress.clear();
    t.run([&] {
        auto created = FrontServer::create(*t.loop, std::move(o));
        REQUIRE_FALSE(created.ok());
        CHECK(contains(created.error().message, "no public name"));
    });
}

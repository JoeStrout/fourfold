// HttpServer over TLS and HTTP/2, its connection and stream caps, slow
// bodies, and graceful shutdown.
#include <nghttp2/nghttp2.h>
#include <csignal>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <map>

#include "doctest.h"
#include "httpserver/http_server.h"
#include "tls/certificates.h"
#include "tls/server_tls.h"

using namespace fourfold;
using std::chrono::milliseconds;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::shared_ptr<ServerTls> testTls(bool http2 = true) {
    auto tls = ServerTls::create(http2);
    REQUIRE(tls.ok());
    auto key = generatePrivateKey();
    REQUIRE(key.ok());
    auto cert = selfSignedCertificate({"example.com"}, key.value(), 2);
    REQUIRE(cert.ok());
    REQUIRE(tls.value()->install(cert.value(), key.value()).ok());
    return tls.value();
}

struct Server {
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<HttpServer> server;
    std::vector<HttpRequest> requests;
    std::vector<Responder> held;   // for handlers that answer later

    explicit Server(HttpServerLimits limits = {}, RequestHandler handler = nullptr) {
        auto created = EventLoop::create();
        REQUIRE(created.ok());
        loop = std::move(created).value();
        if (!handler) {
            handler = [this](HttpRequest request, Responder respond) {
                std::string echo = request.method + " " + request.target + " " + request.body;
                requests.push_back(std::move(request));
                if (requests.back().target.rfind("/later", 0) == 0) {
                    held.push_back(std::move(respond));
                    return;
                }
                respond(HttpResponse::text(200, echo));
            };
        }
        server = std::make_unique<HttpServer>(*loop, std::move(handler), std::move(limits));
    }

    // A new connection; returns the client's end.
    Fd connect(const std::string& peer = "192.0.2.1") {
        auto pair = makeSocketPair();
        REQUIRE(pair.ok());
        server->addConnection(std::move(pair.value().second), peer);
        return std::move(pair.value().first);
    }

    void pump(milliseconds wait = milliseconds(1)) { loop->runOnce(wait); }
};

// Reads what's there on a plain socket, pumping the server.
struct PlainClient {
    Server& server;
    Fd fd;
    std::string received;
    bool eof = false;

    PlainClient(Server& s, const std::string& peer = "192.0.2.1") : server(s), fd(s.connect(peer)) {}

    void send(const std::string& data) {
        size_t sent = 0;
        for (int i = 0; sent < data.size() && i < 1000; i++) {
            auto n = writeSome(fd.get(), data.data() + sent, data.size() - sent);
            if (n) {
                sent += n.value();
            } else {
                server.pump();
            }
        }
    }
    void pump(int times = 1) {
        for (int i = 0; i < times; i++) {
            server.pump();
            char buffer[16384];
            for (;;) {
                auto n = readSome(fd.get(), buffer, sizeof buffer);
                if (!n) break;
                if (n.value() == 0) {
                    eof = true;
                    break;
                }
                received.append(buffer, n.value());
            }
        }
    }
    void until(const std::function<bool()>& done, int tries = 500) {
        for (int i = 0; i < tries && !done() && !eof; i++) pump();
    }
};

// A TLS client on a socket pair, interleaved with the server's loop.
struct TlsClient {
    Server& server;
    Fd fd;
    SSL_CTX* ctx;
    SSL* ssl;
    std::string received;
    bool closed = false;

    TlsClient(Server& s, std::vector<std::string> alpn, const std::string& peer = "192.0.2.1")
        : server(s), fd(s.connect(peer)) {
        // OpenSSL writes with write(): a closed peer must not kill the test (Linux).
        std::signal(SIGPIPE, SIG_IGN);
        ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd.get());
        SSL_set_tlsext_host_name(ssl, "example.com");
        std::string wire;
        for (const std::string& p : alpn) wire += static_cast<char>(p.size()) + p;
        if (!wire.empty()) {
            SSL_set_alpn_protos(ssl, reinterpret_cast<const unsigned char*>(wire.data()),
                                static_cast<unsigned>(wire.size()));
        }
    }
    ~TlsClient() {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
    }

    bool handshake() {
        for (int i = 0; i < 2000; i++) {
            int r = SSL_connect(ssl);
            if (r == 1) return true;
            int e = SSL_get_error(ssl, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                ERR_clear_error();
                return false;
            }
            server.pump();
        }
        return false;
    }
    std::string alpn() {
        const unsigned char* p = nullptr;
        unsigned int n = 0;
        SSL_get0_alpn_selected(ssl, &p, &n);
        return p ? std::string(reinterpret_cast<const char*>(p), n) : "";
    }
    void send(const std::string& data) {
        size_t sent = 0;
        for (int i = 0; sent < data.size() && i < 2000; i++) {
            int n = SSL_write(ssl, data.data() + sent, static_cast<int>(data.size() - sent));
            if (n > 0) {
                sent += static_cast<size_t>(n);
            } else {
                ERR_clear_error();
                server.pump();
            }
        }
    }
    // Pumps the server once, then reads what's decrypted.
    void pump() {
        server.pump();
        char buffer[16384];
        for (;;) {
            int n = SSL_read(ssl, buffer, sizeof buffer);
            if (n > 0) {
                received.append(buffer, static_cast<size_t>(n));
                continue;
            }
            int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL || e == SSL_ERROR_SSL) closed = true;
            ERR_clear_error();
            break;
        }
    }
    void until(const std::function<bool()>& done, int tries = 500) {
        for (int i = 0; i < tries && !done() && !closed; i++) pump();
    }
};

// An HTTP/2 client (nghttp2) over a TlsClient.
struct H2Client {
    struct Response {
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
        bool closed = false;
        uint32_t error = 0;
    };

    TlsClient& tls;
    nghttp2_session* session = nullptr;
    std::map<int32_t, Response> responses;
    bool goaway = false;

    explicit H2Client(TlsClient& t) : tls(t) {
        nghttp2_session_callbacks* cb;
        nghttp2_session_callbacks_new(&cb);
        nghttp2_session_callbacks_set_on_header_callback(
            cb, [](nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t nameLength,
                   const uint8_t* value, size_t valueLength, uint8_t, void* user) {
                auto* self = static_cast<H2Client*>(user);
                std::string n(reinterpret_cast<const char*>(name), nameLength);
                std::string v(reinterpret_cast<const char*>(value), valueLength);
                Response& r = self->responses[frame->hd.stream_id];
                if (n == ":status") {
                    r.status = std::stoi(v);
                } else {
                    r.headers[n] = v;
                }
                return 0;
            });
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            cb, [](nghttp2_session*, uint8_t, int32_t id, const uint8_t* data, size_t length, void* user) {
                static_cast<H2Client*>(user)->responses[id].body.append(reinterpret_cast<const char*>(data), length);
                return 0;
            });
        nghttp2_session_callbacks_set_on_stream_close_callback(
            cb, [](nghttp2_session*, int32_t id, uint32_t error, void* user) {
                Response& r = static_cast<H2Client*>(user)->responses[id];
                r.closed = true;
                r.error = error;
                return 0;
            });
        nghttp2_session_callbacks_set_on_frame_recv_callback(
            cb, [](nghttp2_session*, const nghttp2_frame* frame, void* user) {
                if (frame->hd.type == NGHTTP2_GOAWAY) static_cast<H2Client*>(user)->goaway = true;
                return 0;
            });
        nghttp2_session_client_new(&session, cb, this);
        nghttp2_session_callbacks_del(cb);
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
    }
    ~H2Client() { nghttp2_session_del(session); }

    int32_t request(const std::string& method, const std::string& path,
                    std::vector<std::pair<std::string, std::string>> extra = {}, const std::string* body = nullptr) {
        std::vector<std::pair<std::string, std::string>> fields = {
            {":method", method}, {":path", path}, {":scheme", "https"}, {":authority", "example.com"}};
        for (auto& f : extra) fields.push_back(f);
        std::vector<nghttp2_nv> nva;
        for (auto& [n, v] : fields) {
            nva.push_back({reinterpret_cast<uint8_t*>(const_cast<char*>(n.data())),
                           reinterpret_cast<uint8_t*>(const_cast<char*>(v.data())), n.size(), v.size(),
                           NGHTTP2_NV_FLAG_NONE});
        }
        nghttp2_data_provider provider;
        bodyToSend = body ? *body : "";
        bodyOffset = 0;
        provider.source.ptr = this;
        provider.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t length, uint32_t* flags,
                                    nghttp2_data_source* source, void*) -> ssize_t {
            auto* self = static_cast<H2Client*>(source->ptr);
            size_t n = std::min(length, self->bodyToSend.size() - self->bodyOffset);
            memcpy(buf, self->bodyToSend.data() + self->bodyOffset, n);
            self->bodyOffset += n;
            if (self->bodyOffset == self->bodyToSend.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<ssize_t>(n);
        };
        return nghttp2_submit_request(session, nullptr, nva.data(), nva.size(), body ? &provider : nullptr, this);
    }

    void pump() {
        const uint8_t* data;
        ssize_t n;
        while ((n = nghttp2_session_mem_send(session, &data)) > 0) {
            tls.send(std::string(reinterpret_cast<const char*>(data), static_cast<size_t>(n)));
        }
        tls.pump();
        if (!tls.received.empty()) {
            nghttp2_session_mem_recv(session, reinterpret_cast<const uint8_t*>(tls.received.data()),
                                     tls.received.size());
            tls.received.clear();
        }
    }
    void until(const std::function<bool()>& done, int tries = 500) {
        for (int i = 0; i < tries && !done(); i++) pump();
    }
    bool closed(int32_t id) { return responses.count(id) && responses[id].closed; }

    std::string bodyToSend;
    size_t bodyOffset = 0;
};

RequestHandler streamingHandler(std::vector<std::shared_ptr<StreamWriter>>& writers) {
    return [&writers](HttpRequest, Responder respond) {
        HttpResponse r;
        r.headers.set("Content-Type", "text/event-stream");
        r.stream = [&writers](std::shared_ptr<StreamWriter> w) {
            w->write("event: hello\n\n");
            writers.push_back(std::move(w));
        };
        respond(std::move(r));
    };
}

}  // namespace

TEST_CASE("HttpServer: HTTP/1.1 over TLS") {
    Server s;
    s.server->setTls(testTls());
    TlsClient c(s, {"http/1.1"});
    REQUIRE(c.handshake());
    CHECK_EQ(c.alpn(), "http/1.1");
    c.send("GET /one HTTP/1.1\r\nHost: example.com\r\n\r\n");
    c.until([&] { return contains(c.received, "GET /one "); });
    CHECK_EQ(c.received.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    c.received.clear();
    // The same connection, again; then one that closes it.
    c.send("POST /two HTTP/1.1\r\nHost: example.com\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc");
    c.until([&] { return c.closed; });
    CHECK(contains(c.received, "POST /two abc"));
    CHECK(contains(c.received, "Connection: close\r\n"));
    REQUIRE_EQ(s.requests.size(), 2u);
    CHECK_EQ(s.requests[0].peerAddress, "192.0.2.1");
    c.fd.reset();   // the server waits for the client to close its end
    for (int i = 0; i < 20 && s.server->connectionCount() > 0; i++) s.pump();
    CHECK_EQ(s.server->connectionCount(), 0u);
}

TEST_CASE("HttpServer: TLS refuses plain HTTP, and a stalled handshake times out") {
    HttpServerLimits limits;
    limits.headerTimeout = milliseconds(30);
    Server s(limits);
    s.server->setTls(testTls());
    SUBCASE("plain HTTP") {
        PlainClient c(s);
        c.send("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
        c.until([] { return false; }, 50);
        CHECK(c.eof);
        CHECK_FALSE(contains(c.received, "HTTP/1.1"));
        CHECK(s.requests.empty());
    }
    SUBCASE("nothing sent") {
        PlainClient c(s);
        c.until([] { return false; }, 200);
        CHECK(c.eof);
        CHECK_EQ(s.server->connectionCount(), 0u);
    }
}

TEST_CASE("HttpServer: HTTP/2 when ALPN chooses it") {
    Server s;
    s.server->setTls(testTls());
    TlsClient tls(s, {"h2", "http/1.1"});
    REQUIRE(tls.handshake());
    REQUIRE_EQ(tls.alpn(), "h2");
    H2Client h2(tls);

    SUBCASE("a GET, and the request as the handler sees it") {
        int32_t id = h2.request("GET", "/hello?x=1", {{"cookie", "a=1"}, {"cookie", "b=2"}, {"x-thing", "yes"}});
        h2.until([&] { return h2.closed(id); });
        REQUIRE(h2.closed(id));
        CHECK_EQ(h2.responses[id].status, 200);
        CHECK_EQ(h2.responses[id].body, "GET /hello?x=1 ");
        CHECK_EQ(h2.responses[id].headers["content-length"], "15");
        REQUIRE_EQ(s.requests.size(), 1u);
        const HttpRequest& r = s.requests[0];
        CHECK_EQ(r.versionMajor, 2);
        CHECK_EQ(*r.headers.get("Host"), "example.com");
        CHECK_EQ(*r.headers.get("Cookie"), "a=1; b=2");
        CHECK_EQ(*r.headers.get("X-Thing"), "yes");
        CHECK_EQ(r.peerAddress, "192.0.2.1");
    }
    SUBCASE("streams are handled at once, and answered in any order") {
        int32_t a = h2.request("GET", "/later/a");
        int32_t b = h2.request("GET", "/later/b");
        int32_t c = h2.request("GET", "/now");
        h2.until([&] { return h2.closed(c) && s.held.size() == 2; });
        CHECK(h2.closed(c));
        REQUIRE_EQ(s.held.size(), 2u);
        s.held[1](HttpResponse::text(200, "b"));
        h2.until([&] { return h2.closed(b); });
        CHECK_EQ(h2.responses[b].body, "b");
        CHECK_FALSE(h2.closed(a));
        s.held[0](HttpResponse::text(201, "a"));
        h2.until([&] { return h2.closed(a); });
        CHECK_EQ(h2.responses[a].status, 201);
    }
    SUBCASE("a POST body, and one over the limit") {
        std::string body = "name=x";
        int32_t ok = h2.request("POST", "/form", {{"content-type", "application/x-www-form-urlencoded"}}, &body);
        h2.until([&] { return h2.closed(ok); });
        CHECK_EQ(h2.responses[ok].body, "POST /form name=x");
        std::string big(1024 * 1024 + 10, 'x');
        int32_t tooBig = h2.request("POST", "/form", {}, &big);
        h2.until([&] { return h2.closed(tooBig); }, 2000);
        CHECK_EQ(h2.responses[tooBig].status, 413);
        CHECK_EQ(s.requests.size(), 1u);
    }
    SUBCASE("a declared length over the limit gets 413, and the connection carries on") {
        std::string big(1024 * 1024 + 10, 'x');
        int32_t id = h2.request("POST", "/form", {{"content-length", std::to_string(big.size())}}, &big);
        h2.until([&] { return h2.closed(id); }, 2000);
        CHECK_EQ(h2.responses[id].status, 413);
        CHECK(s.requests.empty());
        int32_t next = h2.request("GET", "/after");
        h2.until([&] { return h2.closed(next); });
        CHECK_EQ(h2.responses[next].status, 200);
        CHECK_FALSE(h2.goaway);
    }
    SUBCASE("HEAD has no body, and connection headers are dropped") {
        Server s2({}, [](HttpRequest, Responder respond) {
            HttpResponse r = HttpResponse::text(200, "body");
            r.headers.set("Connection", "close");
            r.headers.set("X-Mine", "1");
            respond(std::move(r));
        });
        s2.server->setTls(testTls());
        TlsClient t2(s2, {"h2"});
        REQUIRE(t2.handshake());
        H2Client c2(t2);
        int32_t id = c2.request("HEAD", "/");
        c2.until([&] { return c2.closed(id); });
        CHECK_EQ(c2.responses[id].status, 200);
        CHECK(c2.responses[id].body.empty());
        CHECK_EQ(c2.responses[id].headers.count("connection"), 0u);
        CHECK_EQ(c2.responses[id].headers["x-mine"], "1");
    }
}

TEST_CASE("HttpServer: HTTP/2 off gives HTTP/1.1 to an h2 client") {
    Server s;
    s.server->setTls(testTls(false));
    TlsClient tls(s, {"h2", "http/1.1"});
    REQUIRE(tls.handshake());
    CHECK_EQ(tls.alpn(), "http/1.1");
}

TEST_CASE("HttpServer: server-sent events over HTTP/2") {
    std::vector<std::shared_ptr<StreamWriter>> writers;
    HttpServerLimits limits;
    limits.maxStreamsPerAddress = 1;
    Server s(limits, streamingHandler(writers));
    s.server->setTls(testTls());
    TlsClient tls(s, {"h2"});
    REQUIRE(tls.handshake());
    H2Client h2(tls);
    int32_t id = h2.request("GET", "/events");
    h2.until([&] { return contains(h2.responses[id].body, "hello"); });
    REQUIRE_EQ(writers.size(), 1u);
    CHECK_EQ(h2.responses[id].headers["content-type"], "text/event-stream");
    CHECK_EQ(h2.responses[id].headers.count("content-length"), 0u);
    CHECK(writers[0]->write("data: 1\n\n"));
    h2.until([&] { return contains(h2.responses[id].body, "data: 1"); });
    CHECK(contains(h2.responses[id].body, "data: 1\n\n"));
    CHECK_EQ(s.server->streamCount(), 1u);

    // A second stream from the same address is over the cap.
    int32_t second = h2.request("GET", "/events");
    h2.until([&] { return h2.closed(second); });
    CHECK_EQ(h2.responses[second].status, 503);

    // Ending the stream ends it, not the connection.
    writers[0]->close();
    CHECK_FALSE(writers[0]->isOpen());
    h2.until([&] { return h2.closed(id); });
    CHECK(h2.closed(id));
    CHECK_EQ(s.server->streamCount(), 0u);
    CHECK_EQ(s.server->connectionCount(), 1u);
}

TEST_CASE("HttpServer: connection caps, overall and per address") {
    HttpServerLimits limits;
    limits.maxConnectionsPerAddress = 2;
    limits.maxConnections = 4;
    limits.perAddressExempt = {*AddressRange::parse("10.0.0.0/8")};
    Server s(limits);
    PlainClient a(s, "192.0.2.1"), b(s, "192.0.2.1"), c(s, "192.0.2.1");
    c.pump();
    CHECK(c.eof);   // the third from one address
    CHECK_EQ(s.server->connectionCount(), 2u);
    PlainClient p1(s, "10.1.1.1"), p2(s, "10.1.1.1");   // a trusted proxy's
    CHECK_EQ(s.server->connectionCount(), 4u);
    PlainClient d(s, "192.0.2.9");
    d.pump();
    CHECK(d.eof);   // over the total
    // A closed connection frees its place.
    a.fd.reset();
    for (int i = 0; i < 10; i++) s.pump();
    PlainClient e(s, "192.0.2.1");
    e.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    e.until([&] { return contains(e.received, "200 OK"); });
    CHECK(contains(e.received, "200 OK"));
}

TEST_CASE("HttpServer: stream caps over HTTP/1.1, per visitor address") {
    std::vector<std::shared_ptr<StreamWriter>> writers;
    HttpServerLimits limits;
    limits.maxStreamsPerAddress = 1;
    limits.maxStreams = 2;
    Server s(limits, streamingHandler(writers));
    // Behind a proxy: the visitor is in a header.
    s.server->setClientAddress([](const HttpRequest& r) {
        const std::string* v = r.headers.get("X-Visitor");
        return v ? *v : r.peerAddress;
    });
    PlainClient a(s), b(s), c(s), d(s);
    a.send("GET /e HTTP/1.1\r\nHost: x\r\nX-Visitor: one\r\n\r\n");
    a.until([&] { return contains(a.received, "hello"); });
    b.send("GET /e HTTP/1.1\r\nHost: x\r\nX-Visitor: one\r\n\r\n");
    b.until([&] { return b.eof; });
    CHECK(contains(b.received, "HTTP/1.1 503 "));
    CHECK(contains(b.received, "Retry-After: 10"));
    c.send("GET /e HTTP/1.1\r\nHost: x\r\nX-Visitor: two\r\n\r\n");
    c.until([&] { return contains(c.received, "hello"); });
    CHECK_EQ(s.server->streamCount(), 2u);
    d.send("GET /e HTTP/1.1\r\nHost: x\r\nX-Visitor: three\r\n\r\n");
    d.until([&] { return d.eof; });
    CHECK(contains(d.received, "HTTP/1.1 503 "));   // over the total
    // A stream that ends frees its place.
    writers[0]->close();
    a.until([&] { return a.eof; });
    CHECK_EQ(s.server->streamCount(), 1u);
}

TEST_CASE("HttpServer: exempt addresses have no per-address stream cap") {
    std::vector<std::shared_ptr<StreamWriter>> writers;
    HttpServerLimits limits;
    limits.maxStreamsPerAddress = 1;
    limits.perAddressExempt = {*AddressRange::parse("127.0.0.1")};
    Server s(limits, streamingHandler(writers));
    PlainClient a(s, "127.0.0.1"), b(s, "127.0.0.1");
    a.send("GET /e HTTP/1.1\r\nHost: x\r\n\r\n");
    a.until([&] { return contains(a.received, "hello"); });
    b.send("GET /e HTTP/1.1\r\nHost: x\r\n\r\n");
    b.until([&] { return contains(b.received, "hello"); });
    CHECK_EQ(s.server->streamCount(), 2u);
    writers[0]->close();
    a.until([&] { return a.eof; });
    CHECK_EQ(s.server->streamCount(), 1u);
}

TEST_CASE("HttpServer: a body arriving too slowly gets 408") {
    HttpServerLimits limits;
    limits.bodyGrace = milliseconds(20);
    limits.minBodyBytesPerSecond = 100000;
    Server s(limits);
    PlainClient c(s);
    c.send("POST /slow HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\nabc");
    c.until([&] { return c.eof; });
    CHECK_EQ(c.received.rfind("HTTP/1.1 408 ", 0), 0u);
    CHECK(s.requests.empty());

    // One that keeps up is fine.
    limits.minBodyBytesPerSecond = 10;
    Server fast(limits);
    PlainClient f(fast);
    f.send("POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\nabc");
    for (int i = 0; i < 40; i++) f.pump();
    f.send("def");
    f.until([&] { return contains(f.received, "POST /ok abcdef"); });
    CHECK(contains(f.received, "200 OK"));
}

TEST_CASE("HttpServer: graceful shutdown") {
    std::vector<std::shared_ptr<StreamWriter>> writers;
    Server s;
    bool done = false;
    PlainClient idle(s);
    PlainClient busy(s);
    busy.send("GET /later HTTP/1.1\r\nHost: x\r\n\r\n");
    busy.until([&] { return s.held.size() == 1; });
    REQUIRE_EQ(s.held.size(), 1u);

    s.server->shutdown(milliseconds(5000), [&] { done = true; });
    idle.pump(5);
    CHECK(idle.eof);   // idle: closed at once
    // No more connections are taken.
    CHECK_EQ(s.server->connectionCount(), 1u);
    PlainClient late(s);
    late.pump();
    CHECK(late.eof);

    CHECK_FALSE(done);
    s.held[0](HttpResponse::text(200, "finished"));
    busy.until([&] { return busy.eof; });
    CHECK(contains(busy.received, "finished"));
    CHECK(contains(busy.received, "Connection: close"));
    busy.fd.reset();   // the server waits for the client to close its end
    for (int i = 0; i < 10 && !done; i++) s.pump();
    CHECK(done);
}

TEST_CASE("HttpServer: graceful shutdown ends streams, and gives up at the deadline") {
    std::vector<std::shared_ptr<StreamWriter>> writers;
    Server s({}, streamingHandler(writers));
    PlainClient c(s);
    c.send("GET /e HTTP/1.1\r\nHost: x\r\n\r\n");
    c.until([&] { return contains(c.received, "hello"); });
    bool done = false;
    s.server->shutdown(milliseconds(5000), [&] { done = true; });
    c.until([&] { return c.eof; });
    CHECK(c.eof);
    c.fd.reset();
    for (int i = 0; i < 10 && !done; i++) s.pump();
    CHECK(done);

    Server stuck;
    PlainClient d(stuck);
    d.send("GET /later HTTP/1.1\r\nHost: x\r\n\r\n");
    d.until([&] { return stuck.held.size() == 1; });
    bool gaveUp = false;
    stuck.server->shutdown(milliseconds(20), [&] { gaveUp = true; });
    for (int i = 0; i < 200 && !gaveUp; i++) stuck.pump(milliseconds(2));
    CHECK(gaveUp);
    CHECK_EQ(stuck.server->connectionCount(), 0u);
}

TEST_CASE("HttpServer: graceful shutdown of HTTP/2 sends GOAWAY") {
    Server s;
    s.server->setTls(testTls());
    TlsClient tls(s, {"h2"});
    REQUIRE(tls.handshake());
    H2Client h2(tls);
    int32_t id = h2.request("GET", "/later");
    h2.until([&] { return s.held.size() == 1; });
    bool done = false;
    s.server->shutdown(milliseconds(5000), [&] { done = true; });
    h2.until([&] { return h2.goaway; });
    CHECK(h2.goaway);
    CHECK_FALSE(done);
    s.held[0](HttpResponse::text(200, "last"));
    h2.until([&] { return h2.closed(id); });
    CHECK_EQ(h2.responses[id].body, "last");
    for (int i = 0; i < 50 && !done; i++) h2.pump();
    CHECK(done);
}

TEST_CASE("limitConcurrency: runs so many at once, queues the rest, and refuses past the wait") {
    auto created = EventLoop::create();
    REQUIRE(created.ok());
    auto loop = std::move(created).value();
    std::vector<Responder> held;
    AdmissionLimits limits;
    limits.maxRunning = 2;
    limits.maxWaiting = 2;
    limits.maxWait = milliseconds(30);
    RequestHandler h = limitConcurrency(*loop, [&](HttpRequest, Responder r) { held.push_back(std::move(r)); }, limits);
    std::vector<int> statuses(6, 0);
    for (int i = 0; i < 5; i++) {
        h(HttpRequest{}, [&statuses, i](HttpResponse r) { statuses[i] = r.status; });
    }
    CHECK_EQ(held.size(), 2u);    // two running
    CHECK_EQ(statuses[4], 503);   // the fifth: no room to wait
    held[0](HttpResponse::text(200, "a"));
    CHECK_EQ(statuses[0], 200);
    CHECK_EQ(held.size(), 3u);    // the third started
    held[0](HttpResponse::text(200, "again"));   // a second answer is ignored
    CHECK_EQ(held.size(), 3u);
    for (int i = 0; i < 60 && statuses[3] == 0; i++) loop->runOnce(milliseconds(2));
    CHECK_EQ(statuses[3], 503);   // waited too long
    held[1](HttpResponse::text(200, "b"));
    held[2](HttpResponse::text(200, "c"));
    h(HttpRequest{}, [&statuses](HttpResponse r) { statuses[5] = r.status; });
    CHECK_EQ(held.size(), 4u);
    held[3](HttpResponse::text(201, "d"));
    CHECK_EQ(statuses[5], 201);
}

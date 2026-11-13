#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <ctime>

#include "doctest.h"
#include "tls/certificates.h"
#include "tls/server_tls.h"

using namespace fourfold;

namespace {

struct KeyAndCert {
    std::string key, cert;
};

KeyAndCert makeCert(const std::vector<std::string>& names, int days = 30) {
    auto key = generatePrivateKey();
    REQUIRE(key.ok());
    auto cert = selfSignedCertificate(names, key.value(), days);
    REQUIRE(cert.ok());
    return {key.value(), cert.value()};
}

// A TLS client over memory BIOs, talking to a TlsSession with no sockets.
struct Client {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    BIO* in = nullptr;
    BIO* out = nullptr;

    explicit Client(const std::string& sni, std::vector<std::string> alpn = {}, int maxVersion = 0) {
        ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        if (maxVersion) SSL_CTX_set_max_proto_version(ctx, maxVersion);
        ssl = SSL_new(ctx);
        in = BIO_new(BIO_s_mem());
        out = BIO_new(BIO_s_mem());
        BIO_set_mem_eof_return(in, -1);
        SSL_set_bio(ssl, in, out);
        SSL_set_connect_state(ssl);
        if (!sni.empty()) SSL_set_tlsext_host_name(ssl, sni.c_str());
        if (!alpn.empty()) {
            std::string wire;
            for (const std::string& p : alpn) wire += static_cast<char>(p.size()) + p;
            SSL_set_alpn_protos(ssl, reinterpret_cast<const unsigned char*>(wire.data()),
                                static_cast<unsigned>(wire.size()));
        }
    }
    ~Client() {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
    }

    std::string output() {
        char* data = nullptr;
        long n = BIO_get_mem_data(out, &data);
        std::string s(data, n > 0 ? static_cast<size_t>(n) : 0);
        (void)BIO_reset(out);
        return s;
    }
    void input(const std::string& bytes) { BIO_write(in, bytes.data(), static_cast<int>(bytes.size())); }
    std::string read() {
        std::string s;
        char buffer[4096];
        int n;
        while ((n = SSL_read(ssl, buffer, sizeof buffer)) > 0) s.append(buffer, static_cast<size_t>(n));
        return s;
    }
    std::string peerName() {
        X509* cert = SSL_get1_peer_certificate(ssl);
        if (!cert) return "";
        char cn[256] = {0};
        X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof cn);
        X509_free(cert);
        return cn;
    }
};

// Runs the handshake to the end (or failure); returns the server's state.
TlsSession::State handshake(Client& c, TlsSession& s, std::string& plain) {
    TlsSession::State state = TlsSession::State::Open;
    for (int i = 0; i < 20; i++) {
        SSL_do_handshake(c.ssl);
        std::string toServer = c.output();
        state = s.receive(toServer.data(), toServer.size(), plain);
        std::string toClient;
        s.takeOutput(toClient);
        c.input(toClient);
        if (state != TlsSession::State::Open) break;
        if (SSL_is_init_finished(c.ssl) && s.handshakeDone() && toClient.empty() && toServer.empty()) break;
    }
    ERR_clear_error();
    return state;
}

}  // namespace

TEST_CASE("certificates: keys, self-signed certificates, and what's in them") {
    KeyAndCert kc = makeCert({"Example.com", "www.example.com"}, 10);
    auto info = inspectCertificate(kc.cert);
    REQUIRE(info.ok());
    CHECK_EQ(info.value().names, std::vector<std::string>{"example.com", "www.example.com"});
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    CHECK(info.value().notBefore <= now);
    CHECK(info.value().notAfter > now + 9 * 86400);
    CHECK(info.value().notAfter <= now + 10 * 86400 + 60);
    CHECK(info.value().issuer.find("CN=Example.com") != std::string::npos);

    CHECK(checkKeyMatches(kc.cert, kc.key).ok());
    auto other = generatePrivateKey();
    REQUIRE(other.ok());
    Status mismatch = checkKeyMatches(kc.cert, other.value());
    CHECK_FALSE(mismatch.ok());

    CHECK_FALSE(inspectCertificate("not a certificate").ok());
    CHECK_FALSE(selfSignedCertificate({"a,b"}, kc.key, 1).ok());
    CHECK_FALSE(selfSignedCertificate({}, kc.key, 1).ok());
}

TEST_CASE("certificates: a certificate request names every host") {
    auto key = generatePrivateKey();
    REQUIRE(key.ok());
    auto der = certificateRequestDer({"example.com", "www.example.com"}, key.value());
    REQUIRE(der.ok());
    const auto* p = reinterpret_cast<const unsigned char*>(der.value().data());
    X509_REQ* req = d2i_X509_REQ(nullptr, &p, static_cast<long>(der.value().size()));
    REQUIRE(req != nullptr);
    CHECK_EQ(X509_REQ_verify(req, X509_REQ_get0_pubkey(req)), 1);
    STACK_OF(X509_EXTENSION)* exts = X509_REQ_get_extensions(req);
    REQUIRE(exts != nullptr);
    CHECK_EQ(sk_X509_EXTENSION_num(exts), 1);
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    X509_REQ_free(req);
}

TEST_CASE("certificates: host names match exactly or by one wildcard label") {
    CHECK(hostMatches("example.com", "EXAMPLE.com"));
    CHECK_FALSE(hostMatches("example.com", "www.example.com"));
    CHECK(hostMatches("*.example.com", "www.example.com"));
    CHECK_FALSE(hostMatches("*.example.com", "example.com"));
    CHECK_FALSE(hostMatches("*.example.com", "a.b.example.com"));
    CHECK_FALSE(hostMatches("*.example.com", ".example.com"));
}

TEST_CASE("ServerTls: a handshake, then data both ways") {
    auto tls = ServerTls::create();
    REQUIRE(tls.ok());
    KeyAndCert kc = makeCert({"example.com"});
    REQUIRE(tls.value()->install(kc.cert, kc.key).ok());

    TlsSession session(tls.value());
    Client client("example.com", {"h2", "http/1.1"});
    std::string plain;
    CHECK_EQ(handshake(client, session, plain), TlsSession::State::Open);
    CHECK(session.handshakeDone());
    CHECK_EQ(session.protocol(), "h2");
    CHECK_EQ(session.serverName(), "example.com");
    CHECK(SSL_version(client.ssl) == TLS1_3_VERSION);

    SSL_write(client.ssl, "hello", 5);
    std::string bytes = client.output();
    CHECK_EQ(session.receive(bytes.data(), bytes.size(), plain), TlsSession::State::Open);
    CHECK_EQ(plain, "hello");

    session.send("world", 5);
    std::string out;
    session.takeOutput(out);
    CHECK_FALSE(session.hasOutput());
    client.input(out);
    CHECK_EQ(client.read(), "world");

    // close_notify from the client.
    SSL_shutdown(client.ssl);
    bytes = client.output();
    CHECK_EQ(session.receive(bytes.data(), bytes.size(), plain), TlsSession::State::Closed);
}

TEST_CASE("ServerTls: ALPN, TLS 1.2, and HTTP/2 off") {
    KeyAndCert kc = makeCert({"example.com"});
    auto tls = ServerTls::create(false);
    REQUIRE(tls.ok());
    REQUIRE(tls.value()->install(kc.cert, kc.key).ok());
    {
        TlsSession session(tls.value());
        Client client("example.com", {"h2", "http/1.1"}, TLS1_2_VERSION);
        std::string plain;
        CHECK_EQ(handshake(client, session, plain), TlsSession::State::Open);
        CHECK_EQ(session.protocol(), "http/1.1");
        CHECK(SSL_version(client.ssl) == TLS1_2_VERSION);
    }
    {
        TlsSession session(tls.value());
        Client client("example.com");   // no ALPN at all
        std::string plain;
        CHECK_EQ(handshake(client, session, plain), TlsSession::State::Open);
        CHECK_EQ(session.protocol(), "");
    }
    {
        // TLS 1.1 is refused.
        TlsSession session(tls.value());
        Client client("example.com", {}, TLS1_1_VERSION);
        SSL_set_min_proto_version(client.ssl, TLS1_VERSION);
        SSL_set_security_level(client.ssl, 0);
        std::string plain;
        CHECK_EQ(handshake(client, session, plain), TlsSession::State::Failed);
        CHECK_FALSE(session.failure().empty());
    }
}

TEST_CASE("ServerTls: chooses the certificate by SNI, and replaces one") {
    auto tls = ServerTls::create();
    REQUIRE(tls.ok());
    {
        // Nothing installed: the handshake fails.
        TlsSession session(tls.value());
        Client client("example.com");
        std::string plain;
        CHECK_EQ(handshake(client, session, plain), TlsSession::State::Failed);
    }
    KeyAndCert a = makeCert({"a.example"}), b = makeCert({"b.example"}), wild = makeCert({"*.example"});
    REQUIRE(tls.value()->install(a.cert, a.key).ok());
    REQUIRE(tls.value()->install(wild.cert, wild.key).ok());
    REQUIRE(tls.value()->install(b.cert, b.key).ok());
    CHECK_FALSE(tls.value()->install(a.cert, b.key).ok());   // not its key
    for (auto [sni, expect] : {std::pair<const char*, const char*>{"b.example", "b.example"},
                               {"a.example", "a.example"},
                               {"c.example", "*.example"},
                               {"", "a.example"},             // no SNI: the first
                               {"other.test", "a.example"}}) {
        TlsSession session(tls.value());
        Client client(sni);
        std::string plain;
        CHECK_EQ(handshake(client, session, plain), TlsSession::State::Open);
        CHECK_EQ(client.peerName(), expect);
    }
    REQUIRE(tls.value()->certificateFor("b.example"));
    CHECK_EQ(tls.value()->certificateFor("c.example")->names[0], "*.example");
    CHECK_FALSE(tls.value()->certificateFor("other.test"));

    // A renewed certificate for b.example replaces the old one.
    KeyAndCert b2 = makeCert({"b.example"}, 90);
    int64_t before = tls.value()->certificateFor("b.example")->notAfter;
    REQUIRE(tls.value()->install(b2.cert, b2.key).ok());
    CHECK(tls.value()->certificateFor("b.example")->notAfter > before);
}

TEST_CASE("TlsSession: garbage instead of a handshake fails") {
    auto tls = ServerTls::create();
    REQUIRE(tls.ok());
    KeyAndCert kc = makeCert({"example.com"});
    REQUIRE(tls.value()->install(kc.cert, kc.key).ok());
    TlsSession session(tls.value());
    std::string plain;
    std::string junk = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    CHECK_EQ(session.receive(junk.data(), junk.size(), plain), TlsSession::State::Failed);
    CHECK(plain.empty());
}

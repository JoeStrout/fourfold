// `fourfold serve --tls` end to end (M11): HTTPS with the app's own
// certificate, port 80's redirects and challenges, the production headers,
// CSP reports, ACME failing politely, and graceful shutdown.
#include <csignal>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <fstream>
#include <sstream>
#include <thread>

#include "app_server.h"
#include "browser.h"
#include "doctest.h"
#include "temp_dir.h"
#include "tls/certificates.h"

using namespace fourfold;
using namespace fourfold::cli;
using fourfold::test::rawFetch;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct Running {
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;

    Running(const std::string& root, ServerOptions options) {
        server = AppServer::start(root, std::move(options), out, err);
        REQUIRE_MESSAGE(server, err.str());
        serving = std::thread([this] { server->loop().run(); });
    }
    ~Running() {
        if (serving.joinable()) {
            server->loop().stop();
            serving.join();
        }
    }
};

// One HTTPS request (HTTP/1.1, Connection: close) over a blocking socket;
// the whole response, or "" if TLS failed.
std::string httpsFetch(uint16_t port, const std::string& request, const std::string& sni = "example.com",
                       std::string* alpnOut = nullptr) {
        // OpenSSL writes with write(): a closed peer must not kill the test (Linux).
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
    static const unsigned char alpn[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    // Offer only http/1.1 unless asked to see what's chosen.
    if (alpnOut) {
        SSL_set_alpn_protos(ssl, alpn, sizeof alpn);
    } else {
        SSL_set_alpn_protos(ssl, alpn + 3, sizeof alpn - 3);
    }
    std::string response;
    if (SSL_connect(ssl) == 1) {
        if (alpnOut) {
            const unsigned char* p = nullptr;
            unsigned int n = 0;
            SSL_get0_alpn_selected(ssl, &p, &n);
            *alpnOut = p ? std::string(reinterpret_cast<const char*>(p), n) : "";
        }
        if (!request.empty()) {
            SSL_write(ssl, request.data(), static_cast<int>(request.size()));
            char buffer[4096];
            int n;
            while ((n = SSL_read(ssl, buffer, sizeof buffer)) > 0) response.append(buffer, static_cast<size_t>(n));
        } else {
            response = "connected";
        }
    }
    ERR_clear_error();
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return response;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// An app with a page, config.domain, and (unless `acme`) a certificate of
// its own in private/config/.
void makeApp(TempDir& dir, const std::string& tls) {
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body><h1>Hello</h1><script>x=1</script></body></html>");
    dir.write("app/private/config/app.ms", "config = {}\nconfig.domain = \"example.com\"\nconfig.tls = " + tls + "\n");
    auto key = generatePrivateKey().value();
    dir.write("app/private/config/key.pem", key);
    dir.write("app/private/config/cert.pem", selfSignedCertificate({"example.com"}, key, 30).value());
}

ServerOptions tlsOptions() {
    ServerOptions o;
    o.dev = false;
    o.tls = true;
    o.address = "127.0.0.1";
    o.port = 0;
    o.redirectAddress = "127.0.0.1";
    o.redirectPort = 0;
    return o;
}

}  // namespace

TEST_CASE("serve --tls: HTTPS with the app's certificate, and production headers") {
    TempDir dir;
    makeApp(dir, "{\"certFile\": \"cert.pem\", \"keyFile\": \"key.pem\"}");
    Running r(dir.at("app"), tlsOptions());
    uint16_t port = r.server->port();

    std::string alpn;
    CHECK_EQ(httpsFetch(port, "", "example.com", &alpn), "connected");
    CHECK_EQ(alpn, "h2");

    std::string page = httpsFetch(port, "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    REQUIRE_MESSAGE(contains(page, "HTTP/1.1 200 OK"), page);
    CHECK(contains(page, "<h1>Hello</h1>"));
    CHECK(contains(page, "Strict-Transport-Security: max-age=31536000\r\n"));
    CHECK(contains(page, "Content-Security-Policy: default-src 'self'; script-src 'self' 'nonce-"));
    CHECK(contains(page, "report-uri /__fourfold/csp-report"));
    CHECK(contains(page, "Reporting-Endpoints: csp=\"/__fourfold/csp-report\""));
    CHECK(contains(page, "X-Frame-Options: DENY"));
    CHECK(contains(page, "<script nonce=\""));

    // Another host name is refused.
    std::string other = httpsFetch(port, "GET / HTTP/1.1\r\nHost: evil.test\r\nConnection: close\r\n\r\n");
    CHECK(contains(other, "HTTP/1.1 400 "));

    // A CSP report is taken, and logged.
    std::string report =
        "{\"csp-report\":{\"document-uri\":\"https://example.com/\",\"effective-directive\":\"script-src-elem\","
        "\"blocked-uri\":\"inline\",\"line-number\":3}}";
    std::string reported = httpsFetch(port, "POST /__fourfold/csp-report HTTP/1.1\r\nHost: example.com\r\n"
                                            "Content-Type: application/csp-report\r\nConnection: close\r\n"
                                            "Content-Length: " + std::to_string(report.size()) + "\r\n\r\n" + report);
    CHECK(contains(reported, "HTTP/1.1 204 "));
    r.server->log().flush();
    std::string log = slurp(dir.at("app/private/state/") + r.server->log().currentFile());
    CHECK(contains(log, "csp 127.0.0.1 script-src-elem blocked inline on https://example.com/ (line 3)"));
}

TEST_CASE("serve --tls: port 80 redirects to HTTPS, on the app's own names") {
    TempDir dir;
    makeApp(dir, "{\"certFile\": \"cert.pem\", \"keyFile\": \"key.pem\"}");
    Running r(dir.at("app"), tlsOptions());
    uint16_t http = r.server->redirectPort();
    REQUIRE(http != 0);
    std::string https = ":" + std::to_string(r.server->port());

    std::string got = rawFetch(http, "GET /a/b?c=1 HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    CHECK(contains(got, "HTTP/1.1 301 "));
    CHECK(contains(got, "Location: https://example.com" + https + "/a/b?c=1\r\n"));
    got = rawFetch(http, "GET /x HTTP/1.1\r\nHost: evil.test\r\nConnection: close\r\n\r\n");
    CHECK(contains(got, "Location: https://example.com" + https + "/x\r\n"));   // never the visitor's name
    got = rawFetch(http, "POST /form HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    CHECK(contains(got, "HTTP/1.1 308 "));
    got = rawFetch(http, "GET /.well-known/acme-challenge/abc HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    CHECK(contains(got, "HTTP/1.1 404 "));
    got = rawFetch(http, "GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    CHECK(contains(got, "HTTP/1.1 400 "));
}

TEST_CASE("serve --tls: ACME that fails leaves the server running, saying why") {
    TempDir dir;
    // A CA that isn't there.
    makeApp(dir, "{\"acme\": \"https://127.0.0.1:1/directory\"}");
    Running r(dir.at("app"), tlsOptions());
    REQUIRE(r.server->certificates());
    for (int i = 0; i < 200 && r.server->certificates()->status().lastError.empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(contains(r.server->certificates()->status().lastError, "127.0.0.1"));
    CHECK_FALSE(r.server->certificates()->status().current);
    // No certificate: HTTPS can't be had yet; port 80 still answers.
    CHECK_EQ(httpsFetch(r.server->port(), ""), "");
    CHECK(contains(rawFetch(r.server->redirectPort(), "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n"),
                   "301"));
}

TEST_CASE("serve --tls: needs the site's public name, or a certificate") {
    TempDir dir;
    dir.write("app/code/pages/index.html", "hi");
    dir.write("app/private/config/app.ms", "config = {}\n");
    ServerOptions o = tlsOptions();
    o.hosts = {"localhost"};
    std::ostringstream out, err;
    CHECK_FALSE(AppServer::start(dir.at("app"), o, out, err));
    CHECK(contains(err.str(), "config.domain"));

    dir.write("app/private/config/app.ms",
              "config = {}\nconfig.domain = \"example.com\"\nconfig.tls = {\"certFile\": \"nope.pem\", \"keyFile\": \"k.pem\"}\n");
    std::ostringstream out2, err2;
    CHECK_FALSE(AppServer::start(dir.at("app"), tlsOptions(), out2, err2));
    CHECK(contains(err2.str(), "nope.pem"));
}

TEST_CASE("serve: graceful shutdown finishes, then stops the loop") {
    TempDir dir;
    makeApp(dir, "{\"certFile\": \"cert.pem\", \"keyFile\": \"key.pem\"}");
    Running r(dir.at("app"), tlsOptions());
    CHECK(contains(httpsFetch(r.server->port(), "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n"),
                   "200 OK"));
    AppServer* server = r.server.get();
    r.server->loop().post([server] { server->shutdown(std::chrono::seconds(5)); });
    r.serving.join();   // run() returns
    CHECK_EQ(httpsFetch(r.server->port(), ""), "");   // no longer listening
}

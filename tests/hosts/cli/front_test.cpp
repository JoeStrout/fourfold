// M11.1 end to end: `fourfold serve` on a Unix socket, its dropbox written
// by a dropbox writer, behind `fourfold front` over TLS.
#include <csignal>

#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "app_server.h"
#include "doctest.h"
#include "dropboxwriter/dropbox_writer.h"
#include "front/front_server.h"
#include "temp_dir.h"
#include "tls/certificates.h"

using namespace fourfold;
using namespace fourfold::cli;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

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

std::string get(uint16_t port, const std::string& target) {
    return httpsFetch(port, "GET " + target + " HTTP/1.1\r\nHost: club.example.com\r\nConnection: close\r\n\r\n",
                      "club.example.com");
}

}  // namespace

TEST_CASE("front + serve on a Unix socket + dropbox writer: the visitor's address, and the dropbox, get through") {
    TempDir dir;
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body><h1>Club</h1></body></html>");
    dir.write("app/code/pages/t.ms",
              "get.ip = function(req)\n"
              "    return req.ip\n"
              "end function\n"
              "get.log = function(req)\n"
              "    dropbox.write \"visits.log\", \"visit from \" + req.ip\n"
              "    print \"printed\"\n"
              "    return \"logged\"\n"
              "end function\n");
    dir.write("app/private/config/app.ms", "config = {}\nconfig.domain = \"club.example.com\"\n");
    // The writer's folder stands in for the app's dropbox/: what arrives
    // there came through the writer, not from the engine.
    dir.makeDir("owned-by-writer");

    // The dropbox writer.
    auto writerLoop = EventLoop::create().value();
    Fd folder(::open(dir.at("owned-by-writer").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    DropboxWriter writer(*writerLoop, Dropbox(std::move(folder)));
    REQUIRE(writer.adopt(listenUnix(dir.at("d.sock")).value()));
    std::thread writing([&] { writerLoop->run(); });

    // The engine, on its socket.
    ServerOptions o;
    o.dev = false;
    o.https = true;
    o.unixSocket = dir.at("e.sock");
    o.address = "unix:" + o.unixSocket;
    o.dropboxSocket = dir.at("d.sock");
    std::ostringstream out, err;
    auto engine = AppServer::start(dir.at("app"), o, out, err);
    REQUIRE_MESSAGE(engine, err.str());
    REQUIRE_MESSAGE(engine->compiled(), err.str());
    std::thread serving([&] { engine->loop().run(); });

    // front, with a certificate of its own for the app.
    auto frontLoop = EventLoop::create().value();
    FrontOptions f;
    auto key = generatePrivateKey().value();
    dir.write("club.key", key);
    dir.write("club.crt", selfSignedCertificate({"club.example.com"}, key, 30).value());
    FrontApp app;
    app.name = "club";
    app.domains = {"club.example.com"};
    app.socket = o.unixSocket;
    app.certFile = dir.at("club.crt");
    app.keyFile = dir.at("club.key");
    f.config.apps.push_back(app);
    f.stateFolder = dir.at("front-state");
    f.address = "127.0.0.1";
    f.port = 0;
    f.redirectAddress.clear();
    auto front = FrontServer::create(*frontLoop, std::move(f));
    REQUIRE_MESSAGE(front.ok(), front.error().message);
    uint16_t port = front.value()->port();
    std::thread fronting([&] { frontLoop->run(); });

    std::string home = get(port, "/");
    CHECK(contains(home, "HTTP/1.1 200 OK"));
    CHECK(contains(home, "<h1>Club</h1>"));
    CHECK(contains(home, "Strict-Transport-Security: "));   // the engine's headers, passed on
    CHECK(contains(home, "Set-Cookie: __Host-"));            // HTTPS cookies: front says proto=https

    // req.ip is the visitor's, not the socket's.
    std::string ip = get(port, "/t/ip");
    CHECK(contains(ip, "\r\n\r\n127.0.0.1"));

    std::string logged = get(port, "/t/log");
    CHECK(contains(logged, "logged"));
    for (int i = 0; i < 100 && !contains(slurp(dir.at("owned-by-writer/print.log")), "printed"); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(slurp(dir.at("owned-by-writer/visits.log")), "visit from 127.0.0.1\n");
    CHECK(contains(slurp(dir.at("owned-by-writer/print.log")), "printed"));
    CHECK_FALSE(std::ifstream(dir.at("app/dropbox/visits.log")).good());

    // The engine's log names its socket and the writer's.
    engine->log().flush();
    std::string log = slurp(dir.at("app/private/state/") + engine->log().currentFile());
    CHECK(contains(log, "listening on " + o.unixSocket));
    CHECK(contains(log, "dropbox writer at " + o.dropboxSocket));
    CHECK(contains(log, "127.0.0.1 GET /t/ip 200"));

    frontLoop->stop();
    fronting.join();
    front.value().reset();
    engine->loop().stop();
    serving.join();
    engine.reset();
    writerLoop->stop();
    writing.join();
}

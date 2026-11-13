// FrontServer: `fourfold front`, the multi-app router (overview §8, several
// apps on one server). It owns ports 443 and 80 for every app the server
// config lists, and nothing else:
//
// - HTTPS (HTTP/2 where the browser offers it), with a certificate per app
//   chosen by SNI: from the app's own files, or from an ACME CA, kept in
//   front's state folder (<state>/apps/<name>/tls/), one account per app.
// - Each request goes, by its Host, to that app's engine over the engine's
//   Unix socket (Upstream); an unknown host gets a 421.
// - Port 80 answers ACME's challenges and otherwise redirects to HTTPS, for
//   the apps' own names only.
// - The connection limits of overview §8 apply here, per visitor; each
//   engine applies its own again, by the Forwarded visitor.
//
// It reads no app's code, data or config: it runs as its own OS user, and
// only needs to connect to the engines' sockets.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "acme/certificate_manager.h"
#include "config/app_config.h"
#include "front/upstream.h"
#include "httpserver/http_server.h"
#include "partition/data_dir.h"
#include "platform/event_loop.h"
#include "tls/server_tls.h"

namespace fourfold {

struct FrontOptions {
    FrontConfig config;
    // Where ACME accounts and certificates are kept (created 0700 if missing).
    std::string stateFolder = "/var/lib/fourfold-front";
    // HTTPS: a socket from systemd, or address:port.
    std::shared_ptr<Fd> listenSocket;
    std::string address = "::";
    uint16_t port = 443;
    // Port 80 (redirects and ACME's challenges): a socket from systemd, or
    // redirectAddress:redirectPort; neither: none.
    std::shared_ptr<Fd> redirectSocket;
    std::string redirectAddress = "::";
    uint16_t redirectPort = 80;
    UpstreamLimits upstream;
    // Each line front has to say (certificates, failures).
    std::function<void(const std::string&)> log;
    // For tests: the ACME CA's HTTP (otherwise libcurl).
    std::function<std::unique_ptr<AcmeHttp>()> acmeHttp;
};

class FrontServer {
public:
    // Sets up TLS (installing certificates on disk or from files at once),
    // the routes and the listeners. Certificates from ACME are fetched in
    // the background once start() is called.
    static Result<std::unique_ptr<FrontServer>> create(EventLoop& loop, FrontOptions options);
    ~FrontServer();

    FrontServer(const FrontServer&) = delete;
    FrontServer& operator=(const FrontServer&) = delete;

    // Starts the certificate managers.
    void start();
    // Stops taking connections, ends streams, lets requests in flight
    // finish (up to `deadline`), then calls `done`. On the loop's thread.
    void shutdown(std::chrono::milliseconds deadline, std::function<void()> done);

    uint16_t port() const { return https_->port(); }
    uint16_t redirectPort() const { return redirect_ ? redirect_->port() : 0; }
    ServerTls& tls() { return *tls_; }
    HttpServer& https() { return *https_; }
    // The app serving `host` (a Host field, with or without a port), or null.
    Upstream* route(const std::string& host) const;
    const std::vector<std::shared_ptr<Upstream>>& upstreams() const { return upstreams_; }
    CertificateManager* certificates(const std::string& app) const;

private:
    explicit FrontServer(FrontOptions options) : options_(std::move(options)) {}
    Status setUpCertificates();
    void handle(HttpRequest request, Responder respond);
    void redirect(HttpRequest request, Responder respond);
    void say(const std::string& line) const;

    FrontOptions options_;
    std::shared_ptr<ServerTls> tls_;
    std::vector<std::shared_ptr<Upstream>> upstreams_;   // in options_.config.apps' order
    std::map<std::string, Upstream*> byHost_;
    std::vector<std::unique_ptr<StateDir>> states_;   // per app, for its manager
    std::map<std::string, std::unique_ptr<CertificateManager>> managers_;   // by app
    std::unique_ptr<HttpServer> https_;
    std::unique_ptr<HttpServer> redirect_;
};

// The host name in a Host field: lowercase, without a port.
std::string hostOfField(const std::string& host);

}  // namespace fourfold

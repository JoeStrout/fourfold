#include "front/front_server.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "acme/acme_client.h"
#include "platform/socket.h"

namespace fourfold {

namespace {

const char* const kChallengePrefix = "/.well-known/acme-challenge/";

Error osError(const std::string& what) {
    int e = errno;
    return makeError(e == EACCES || e == EPERM ? ErrorCode::Denied : ErrorCode::IO, what + ": " + std::strerror(e));
}

// A folder, made 0700 if missing.
Result<Fd> openFolder(const std::string& path) {
    if (mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) return osError(path);
    Fd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!fd.valid()) return osError(path);
    return fd;
}

Result<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return makeError(ErrorCode::NotFound, path + ": can't be read");
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

std::string hostOfField(const std::string& host) {
    std::string h = !host.empty() && host[0] == '[' ? host.substr(0, host.find(']') + 1) : host.substr(0, host.find(':'));
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h;
}

Result<std::unique_ptr<FrontServer>> FrontServer::create(EventLoop& loop, FrontOptions options) {
    std::unique_ptr<FrontServer> f(new FrontServer(std::move(options)));
    const FrontConfig& config = f->options_.config;
    if (config.apps.empty()) return makeError(ErrorCode::Invalid, "the server config lists no apps (server.apps)");

    for (const FrontApp& app : config.apps) {
        auto up = Upstream::create(loop, app.name, app.socket, f->options_.upstream);
        for (const std::string& d : app.domains) f->byHost_[d] = up.get();
        f->upstreams_.push_back(std::move(up));
    }

    auto tls = ServerTls::create(config.tls.http2);
    if (!tls) return tls.error();
    f->tls_ = std::move(tls).value();

    // The limits of overview §8, as an engine facing the internet has them.
    const ServerConfig& sc = config.server;
    HttpServerLimits limits;
    limits.maxConnections = sc.connections ? sc.connections : connectionsForOpenFiles();
    limits.maxConnectionsPerAddress = sc.connectionsPerAddress;
    limits.perAddressExempt.push_back(*AddressRange::parse("127.0.0.0/8"));
    limits.perAddressExempt.push_back(*AddressRange::parse("::1"));
    limits.maxStreams = sc.streams;
    limits.maxStreamsPerAddress = sc.streamsPerAddress;
    limits.headerTimeout = sc.headerTimeout;
    limits.minBodyBytesPerSecond = sc.minUploadBytesPerSecond;
    limits.idleTimeout = sc.keepAlive;
    FrontServer* self = f.get();
    f->https_ = std::make_unique<HttpServer>(
        loop, [self](HttpRequest request, Responder respond) { self->handle(std::move(request), std::move(respond)); },
        limits);
    f->https_->setTls(f->tls_);

    if (Status s = f->setUpCertificates(); !s) return s.error();

    if (f->options_.listenSocket && f->options_.listenSocket->valid()) {
        if (Status s = f->https_->adopt(std::move(*f->options_.listenSocket)); !s) return s.error();
    } else if (Status s = f->https_->listen(f->options_.address, f->options_.port); !s) {
        return makeError(s.error().code, "can't listen on " + f->options_.address + " port " +
                                             std::to_string(f->options_.port) + ": " + s.error().message);
    }

    bool redirectSocket = f->options_.redirectSocket && f->options_.redirectSocket->valid();
    if (redirectSocket || !f->options_.redirectAddress.empty()) {
        HttpServerLimits small;
        small.maxBodyBytes = 16 * 1024;
        small.maxUploadBodyBytes = 16 * 1024;
        small.maxConnections = 1024;
        small.maxConnectionsPerAddress = sc.connectionsPerAddress;
        f->redirect_ = std::make_unique<HttpServer>(
            loop,
            [self](HttpRequest request, Responder respond) { self->redirect(std::move(request), std::move(respond)); },
            small);
        Status s = redirectSocket ? f->redirect_->adopt(std::move(*f->options_.redirectSocket))
                                  : f->redirect_->listen(f->options_.redirectAddress, f->options_.redirectPort);
        if (!s) {
            return makeError(s.error().code, "can't listen on " + f->options_.redirectAddress + " port " +
                                                 std::to_string(f->options_.redirectPort) + ": " + s.error().message);
        }
    }
    return f;
}

Status FrontServer::setUpCertificates() {
    const FrontConfig& config = options_.config;
    bool needState = false;
    for (const FrontApp& app : config.apps) needState = needState || app.certFile.empty();
    std::string root = options_.stateFolder;
    if (needState) {
        auto top = openFolder(root);
        if (!top) return top.error();
        auto apps = openFolder(root + "/apps");
        if (!apps) return apps.error();
    }
    for (const FrontApp& app : config.apps) {
        if (!app.certFile.empty()) {
            auto cert = readFile(app.certFile);
            auto key = readFile(app.keyFile);
            if (!cert || !key) return !cert ? cert.error() : key.error();
            if (Status s = tls_->install(cert.value(), key.value()); !s) {
                return makeError(s.error().code, "server.apps." + app.name + "'s certificate: " + s.error().message);
            }
            auto info = inspectCertificate(cert.value());
            if (info && info.value().notAfter < static_cast<int64_t>(std::time(nullptr)) + 14 * 86400) {
                say("Warning: " + app.name + "'s certificate (" + app.certFile + ") expires within two weeks");
            }
            continue;
        }
        // From the ACME CA, for the app's public names.
        CertificateManagerOptions o;
        for (const std::string& d : app.domains) {
            if (CertificateManager::certifiable(d)) o.names.push_back(d);
        }
        if (o.names.empty()) {
            return makeError(ErrorCode::Invalid, "server.apps." + app.name +
                                                     " has no public name to get a certificate for; give it "
                                                     "certFile and keyFile instead");
        }
        std::string folder = root + "/apps/" + app.name;
        auto fd = openFolder(folder);
        if (!fd) return fd.error();
        states_.push_back(std::make_unique<StateDir>(std::move(fd).value(), folder));
        o.acme.directoryUrl = config.tls.acmeDirectory.empty() ? kLetsEncryptDirectory : config.tls.acmeDirectory;
        o.acme.email = config.tls.email;
        o.acmeCaBundle = config.tls.acmeCaBundle;
        if (options_.acmeHttp) o.http = options_.acmeHttp();
        std::shared_ptr<ServerTls> serverTls = tls_;
        o.install = [serverTls](const std::string& chain, const std::string& key) { return serverTls->install(chain, key); };
        std::string name = app.name;
        FrontServer* self = this;
        o.log = [self, name](const std::string& line) { self->say("TLS (" + name + "): " + line); };
        auto manager = CertificateManager::create(*states_.back(), std::move(o));
        if (!manager) return manager.error();
        managers_[app.name] = std::move(manager).value();
    }
    return {};
}

void FrontServer::start() {
    for (auto& [name, manager] : managers_) manager->start();
}

Upstream* FrontServer::route(const std::string& host) const {
    auto it = byHost_.find(hostOfField(host));
    return it == byHost_.end() ? nullptr : it->second;
}

CertificateManager* FrontServer::certificates(const std::string& app) const {
    auto it = managers_.find(app);
    return it == managers_.end() ? nullptr : it->second.get();
}

void FrontServer::handle(HttpRequest request, Responder respond) {
    const std::string* host = request.headers.get("Host");
    Upstream* up = host ? route(*host) : nullptr;
    if (!up) {
        // Not a site served here (or a request with no Host).
        HttpResponse r = HttpResponse::text(421, "Misdirected Request: no site here by that name\n");
        r.headers.set("Cache-Control", "no-store");
        respond(std::move(r));
        return;
    }
    std::string visitor = request.peerAddress;
    up->forward(std::move(request), visitor, "https", std::move(respond));
}

void FrontServer::redirect(HttpRequest request, Responder respond) {
    const std::string& target = request.target;
    size_t prefix = std::char_traits<char>::length(kChallengePrefix);
    if (target.compare(0, prefix, kChallengePrefix) == 0) {
        std::string token = target.substr(prefix);
        std::optional<std::string> answer;
        for (auto& [name, manager] : managers_) {
            if (!answer) answer = manager->challenges().get(token);
        }
        HttpResponse r = answer ? HttpResponse::text(200, *answer) : HttpResponse::text(404, "Not Found\n");
        if (answer) r.headers.set("Content-Type", "application/octet-stream");
        respond(std::move(r));
        return;
    }
    // Only to the apps' own names, never a name a visitor made up.
    const std::string* field = request.headers.get("Host");
    std::string host = field ? hostOfField(*field) : std::string();
    if (!byHost_.count(host)) {
        respond(HttpResponse::text(421, "Misdirected Request: no site here by that name\n"));
        return;
    }
    if (target.empty() || target[0] != '/') {
        respond(HttpResponse::text(400, "Bad Request\n"));
        return;
    }
    bool safe = request.method == "GET" || request.method == "HEAD";
    uint16_t httpsPort = https_->port();
    HttpResponse r = HttpResponse::text(safe ? 301 : 308, "Moved to HTTPS\n");
    r.headers.set("Location",
                  "https://" + host + (httpsPort == 443 || httpsPort == 0 ? "" : ":" + std::to_string(httpsPort)) + target);
    respond(std::move(r));
}

void FrontServer::shutdown(std::chrono::milliseconds deadline, std::function<void()> done) {
    if (redirect_) redirect_->shutdown(std::chrono::milliseconds(1000), [] {});
    // The visitors' connections go first (streams end with them); then
    // what's left of the engines'.
    std::vector<std::shared_ptr<Upstream>> ups = upstreams_;
    https_->shutdown(deadline, [ups, done] {
        for (auto& u : ups) u->closeAll();
        if (done) done();
    });
}

FrontServer::~FrontServer() {
    managers_.clear();
    redirect_.reset();
    https_.reset();
    for (auto& u : upstreams_) u->closeAll();
}

void FrontServer::say(const std::string& line) const {
    if (options_.log) options_.log(line);
}

}  // namespace fourfold

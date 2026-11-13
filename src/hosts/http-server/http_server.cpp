#include "httpserver/http_server.h"

#include "connection.h"

namespace fourfold {

HttpServer::HttpServer(EventLoop& loop, RequestHandler handler, HttpServerLimits limits)
    : loop_(loop), handler_(std::move(handler)), limits_(std::move(limits)) {}

HttpServer::~HttpServer() {
    loop_.cancelTimer(shutdownTimer_);
    shutdownDone_ = nullptr;
    for (const Fd& fd : listeners_) loop_.unwatch(fd.get());
    // close() removes each connection from connections_, so work on a copy.
    auto connections = std::move(connections_);
    connections_.clear();
    for (auto& entry : connections) entry.second->close();
}

Status HttpServer::listen(const std::string& host, uint16_t port) {
    auto fd = listenTcp(host, port);
    if (!fd) return fd.error();
    return adopt(std::move(fd).value());
}

Status HttpServer::adopt(Fd listening) {
    if (Status s = setNonBlocking(listening.get()); !s) return s;
    auto actualPort = localPort(listening.get());
    int raw = listening.get();
    if (Status s = loop_.watch(raw, io::kReadable, [this, raw](uint32_t) { onAcceptable(raw); }); !s) {
        return s;
    }
    if (listeners_.empty() && actualPort) port_ = actualPort.value();
    listeners_.push_back(std::move(listening));
    return {};
}

void HttpServer::onAcceptable(int fd) {
    for (;;) {
        auto accepted = acceptConnection(fd);
        // An accept error (e.g. out of file descriptors) leaves the
        // connection queued; the loop reports the socket ready again.
        if (!accepted || !accepted.value()) return;
        addConnection(std::move(accepted.value()->fd), std::move(accepted.value()->peerAddress));
    }
}

bool HttpServer::exempt(const std::string& address) const {
    for (const AddressRange& r : limits_.perAddressExempt) {
        if (r.contains(address)) return true;
    }
    return false;
}

void HttpServer::addConnection(Fd fd, std::string peerAddress) {
    if (shuttingDown_ || connections_.size() >= limits_.maxConnections) return;   // fd closes here
    if (!exempt(peerAddress)) {
        size_t& count = perAddress_[peerAddress];
        if (count >= limits_.maxConnectionsPerAddress) return;
        count++;
    }
    auto connection = std::make_shared<HttpConnection>(*this, std::move(fd), std::move(peerAddress));
    connections_[connection.get()] = connection;
    connection->start();
}

void HttpServer::removeConnection(HttpConnection* connection) {
    auto it = perAddress_.find(connection->peerAddress());
    if (it != perAddress_.end() && --it->second == 0) perAddress_.erase(it);
    connections_.erase(connection);
    if (shuttingDown_) checkShutdown();
}

bool HttpServer::startStream(const std::string& address) {
    if (streams_ >= limits_.maxStreams) return false;
    if (exempt(address)) {
        streams_++;
        return true;
    }
    size_t& count = streamsPerAddress_[address];
    if (count >= limits_.maxStreamsPerAddress) return false;
    count++;
    streams_++;
    return true;
}

void HttpServer::endStream(const std::string& address) {
    if (streams_ > 0) streams_--;
    if (exempt(address)) return;
    auto it = streamsPerAddress_.find(address);
    if (it != streamsPerAddress_.end() && --it->second == 0) streamsPerAddress_.erase(it);
}

void HttpServer::shutdown(std::chrono::milliseconds deadline, std::function<void()> done) {
    shuttingDown_ = true;
    shutdownDone_ = std::move(done);
    for (const Fd& fd : listeners_) loop_.unwatch(fd.get());
    listeners_.clear();
    shutdownTimer_ = loop_.addTimer(deadline, [this] {
        shutdownTimer_ = 0;
        auto connections = connections_;
        for (auto& entry : connections) entry.second->close();
        checkShutdown();
    });
    auto connections = connections_;   // goAway may close (and remove) some
    for (auto& entry : connections) entry.second->goAway();
    checkShutdown();
}

void HttpServer::checkShutdown() {
    if (!connections_.empty() || !shutdownDone_) return;
    loop_.cancelTimer(shutdownTimer_);
    shutdownTimer_ = 0;
    auto done = std::move(shutdownDone_);
    shutdownDone_ = nullptr;
    // Not from inside a connection's callback.
    loop_.post(std::move(done));
}

}  // namespace fourfold

#include "httpserver/http_server.h"

#include "connection.h"

namespace fourfold {

HttpServer::HttpServer(EventLoop& loop, RequestHandler handler, HttpServerLimits limits)
    : loop_(loop), handler_(std::move(handler)), limits_(limits) {}

HttpServer::~HttpServer() {
    if (listenFd_.valid()) loop_.unwatch(listenFd_.get());
    // close() removes each connection from connections_, so work on a copy.
    auto connections = std::move(connections_);
    connections_.clear();
    for (auto& entry : connections) entry.second->close();
}

Status HttpServer::listen(const std::string& host, uint16_t port) {
    auto fd = listenTcp(host, port);
    if (!fd) return fd.error();
    auto actualPort = localPort(fd.value().get());
    if (!actualPort) return actualPort.error();
    if (Status s = loop_.watch(fd.value().get(), io::kReadable, [this](uint32_t) { onAcceptable(); });
        !s) {
        return s;
    }
    listenFd_ = std::move(fd).value();
    port_ = actualPort.value();
    return {};
}

void HttpServer::onAcceptable() {
    for (;;) {
        auto accepted = acceptConnection(listenFd_.get());
        // An accept error (e.g. out of file descriptors) leaves the
        // connection queued; the loop reports the socket ready again.
        if (!accepted || !accepted.value()) return;
        addConnection(std::move(accepted.value()->fd), std::move(accepted.value()->peerAddress));
    }
}

void HttpServer::addConnection(Fd fd, std::string peerAddress) {
    if (connections_.size() >= limits_.maxConnections) return;   // fd closes here
    auto connection = std::make_shared<HttpConnection>(*this, std::move(fd), std::move(peerAddress));
    connections_[connection.get()] = connection;
    connection->start();
}

void HttpServer::removeConnection(HttpConnection* connection) {
    connections_.erase(connection);
}

}  // namespace fourfold

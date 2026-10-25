// HttpServer: HTTP/1.1 over TCP (or any stream socket), on an EventLoop.
//
// It reads requests under limits, parses them with llhttp, hands each one to
// a RequestHandler as an HttpRequest, and writes back the HttpResponse the
// handler gives it. Requests on one connection are handled one at a time, in
// order; keep-alive and pipelining are supported.
//
// The handler may respond at once, or later (e.g. after the script thread
// runs). Either way, it must call the Responder on the loop thread. If the
// connection has closed meanwhile, the response is dropped.
//
// A response with a `stream` (server-sent events) is sent without a
// Content-Length, and ends the connection when the stream closes. Streams
// aren't subject to the request or idle timeouts.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "base/result.h"
#include "http/message.h"
#include "platform/event_loop.h"
#include "platform/socket.h"

namespace fourfold {

struct HttpServerLimits {
    size_t maxHeaderBytes = 16 * 1024;     // request line plus headers (431, or 414 for the URL)
    size_t maxHeaderCount = 100;           // 431
    size_t maxBodyBytes = 1024 * 1024;     // 413
    size_t maxConnections = 1024;          // more are closed at once
    // Time allowed to receive a whole request, from its first byte (408).
    std::chrono::milliseconds requestTimeout{30000};
    // Time an idle keep-alive connection is kept open.
    std::chrono::milliseconds idleTimeout{60000};
    // A streamed response (server-sent events) is closed if this much of it
    // is waiting for a client that isn't reading.
    size_t maxStreamBufferBytes = 1024 * 1024;
};

using Responder = std::function<void(HttpResponse)>;
using RequestHandler = std::function<void(HttpRequest, Responder)>;

class HttpConnection;   // private

class HttpServer {
public:
    HttpServer(EventLoop& loop, RequestHandler handler, HttpServerLimits limits = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Starts accepting connections on host:port (a numeric address; port 0
    // picks a free one, reported by port()).
    Status listen(const std::string& host, uint16_t port);
    uint16_t port() const { return port_; }

    // Serves an already-connected, non-blocking socket. listen() uses this for
    // each connection it accepts; tests use it with a socket pair.
    void addConnection(Fd fd, std::string peerAddress);

    size_t connectionCount() const { return connections_.size(); }

private:
    friend class HttpConnection;
    void onAcceptable();
    void removeConnection(HttpConnection* connection);

    EventLoop& loop_;
    RequestHandler handler_;
    HttpServerLimits limits_;
    Fd listenFd_;
    uint16_t port_ = 0;
    std::unordered_map<HttpConnection*, std::shared_ptr<HttpConnection>> connections_;
};

}  // namespace fourfold

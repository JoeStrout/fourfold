// Upstream: fourfold front's way to one app's engine, HTTP/1.1 over the
// engine's Unix-domain socket (overview §8, several apps on one server).
//
// Each request goes on a connection of its own until its response is in;
// connections are kept for reuse while idle (a short while, under the
// engine's own keep-alive time). The request is passed on as it came, less
// the hop-by-hop fields and any forwarding fields the visitor sent, plus a
// Forwarded field naming the visitor, which the engine trusts because it
// arrives on its Unix socket.
//
// A response with a Content-Length (up to maxBufferedBody) is collected,
// then answered whole, so the visitor's connection stays open. Anything
// else, server-sent events above all, is streamed: the visitor gets the
// headers at once and the body as it comes, until the engine closes.
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "http/message.h"
#include "httpserver/http_server.h"
#include "platform/event_loop.h"

namespace fourfold {

struct UpstreamLimits {
    // Connections to the engine at once (requests in flight, and streams);
    // past it, a 503.
    size_t maxConnections = 1024;
    // Idle connections kept, and for how long.
    size_t maxIdle = 32;
    std::chrono::milliseconds idleTimeout{20000};
    // From sending the request until the response's headers are in: 504.
    std::chrono::milliseconds responseTimeout{120000};
    // A larger response (or one without a Content-Length) is streamed.
    size_t maxBufferedBody = 16 * 1024 * 1024;
    // Response headers from the engine, at most.
    size_t maxHeaderBytes = 64 * 1024;
};

class UpstreamConnection;   // private

class Upstream : public std::enable_shared_from_this<Upstream> {
public:
    // `name` is the app's, for error messages.
    static std::shared_ptr<Upstream> create(EventLoop& loop, std::string name, std::string socketPath,
                                            UpstreamLimits limits = {});
    ~Upstream();

    Upstream(const Upstream&) = delete;
    Upstream& operator=(const Upstream&) = delete;

    // Sends `request` (from `visitor`, for Forwarded) to the engine, and
    // answers `respond` with its response, or a 502, 503 or 504 of front's
    // own. On the loop's thread.
    void forward(HttpRequest request, const std::string& visitor, const std::string& proto, Responder respond);

    // Closes every connection: idle ones, ones waiting for a response (those
    // get a 502), and streams.
    void closeAll();

    const std::string& name() const { return name_; }
    const std::string& socketPath() const { return socketPath_; }
    size_t connectionCount() const { return active_.size() + idle_.size(); }
    size_t idleCount() const { return idle_.size(); }

private:
    friend class UpstreamConnection;
    Upstream(EventLoop& loop, std::string name, std::string socketPath, UpstreamLimits limits)
        : loop_(loop), name_(std::move(name)), socketPath_(std::move(socketPath)), limits_(limits) {}

    // A connection whose exchange is done: kept if it may be reused.
    void release(UpstreamConnection* c, bool reusable);
    void forget(UpstreamConnection* c);
    void sendOn(std::shared_ptr<UpstreamConnection> c, std::string wire, Responder respond, bool retryable);

    EventLoop& loop_;
    std::string name_;
    std::string socketPath_;
    UpstreamLimits limits_;
    std::vector<std::shared_ptr<UpstreamConnection>> active_;
    std::vector<std::shared_ptr<UpstreamConnection>> idle_;   // most recently used last
};

// The request as front sends it on (exposed for tests): HTTP/1.1, HEAD sent
// as GET (the visitor's server leaves out the body, and keeps the right
// Content-Length), hop-by-hop and forwarding fields dropped, Forwarded added.
std::string upstreamRequest(const HttpRequest& request, const std::string& visitor, const std::string& proto);

// A Forwarded element for `visitor` (RFC 7239): IPv6 in brackets and
// quotes; an IPv4-mapped IPv6 address as plain IPv4.
std::string forwardedElement(const std::string& visitor, const std::string& proto, const std::string& host);

}  // namespace fourfold

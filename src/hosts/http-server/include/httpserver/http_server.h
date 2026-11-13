// HttpServer: HTTP/1.1, and HTTP/2 over TLS, on an EventLoop.
//
// It reads requests under limits (overview §8, Connection and request
// limits), parses them (llhttp; nghttp2 for HTTP/2), hands each one to a
// RequestHandler as an HttpRequest, and writes back the HttpResponse the
// handler gives it. On an HTTP/1.1 connection, requests are handled one at a
// time, in order; keep-alive and pipelining are supported. On HTTP/2, a
// connection's streams are handled concurrently.
//
// With a ServerTls (setTls), every connection is TLS, and a client that
// offers "h2" in ALPN speaks HTTP/2.
//
// The handler may respond at once, or later (e.g. after the script thread
// runs). Either way, it must call the Responder on the loop thread. If the
// connection has closed meanwhile, the response is dropped.
//
// A response with a `stream` (server-sent events) is sent without a
// Content-Length, and ends the connection (on HTTP/2, the stream) when the
// stream closes. Streams aren't subject to the request or idle timeouts, and
// are counted against their own caps.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/result.h"
#include "http/message.h"
#include "platform/address.h"
#include "platform/event_loop.h"
#include "platform/socket.h"

namespace fourfold {

class ServerTls;

struct HttpServerLimits {
    size_t maxHeaderBytes = 16 * 1024;     // request line plus headers (431, or 414 for the URL)
    size_t maxHeaderCount = 100;           // 431
    size_t maxBodyBytes = 1024 * 1024;     // 413
    // A multipart/form-data body (a form with file uploads) may be this
    // large instead; the engine then holds each route to its declared
    // uploads' sizes (overview §8, Connection and request limits).
    size_t maxUploadBodyBytes = 33 * 1024 * 1024;

    // Connections: more are closed at once. Per address, not counting the
    // addresses in perAddressExempt (a proxy, whose connections carry many
    // visitors; these are exempt from the per-address stream cap too), nor
    // a Unix-domain socket's (kUnixPeer).
    size_t maxConnections = 1024;
    size_t maxConnectionsPerAddress = 100;
    std::vector<AddressRange> perAddressExempt;

    // Open streamed responses (server-sent events), counted separately
    // since they're long-lived: past either cap, the response is a 503.
    // Per visitor address (setClientAddress), not per connection.
    size_t maxStreams = 1000;
    size_t maxStreamsPerAddress = 20;

    // From a request's first byte (or a new connection's, the TLS handshake
    // included) until its headers are in (408; slowloris).
    std::chrono::milliseconds headerTimeout{10000};
    // A request body must arrive at this average rate at least, once the
    // grace period after its headers is over (408; slow bodies).
    size_t minBodyBytesPerSecond = 1024;
    std::chrono::milliseconds bodyGrace{10000};
    // A response must make progress this often, or the client (not
    // reading) is dropped.
    std::chrono::milliseconds sendTimeout{30000};
    // Time an idle keep-alive connection is kept open.
    std::chrono::milliseconds idleTimeout{60000};
    // A streamed response (server-sent events) is closed if this much of it
    // is waiting for a client that isn't reading.
    size_t maxStreamBufferBytes = 1024 * 1024;
    // HTTP/2: streams open at once on one connection.
    uint32_t maxConcurrentStreams = 100;
};

using Responder = std::function<void(HttpResponse)>;
using RequestHandler = std::function<void(HttpRequest, Responder)>;

// Concurrent requests (overview §8, Connection and request limits): at most
// maxRunning with the handler at once; more wait, in order, up to maxWaiting
// of them for up to maxWait each, then get a 503.
struct AdmissionLimits {
    size_t maxRunning = 64;
    size_t maxWaiting = 1000;
    std::chrono::milliseconds maxWait{5000};
};

// `handler`, admitting requests under `limits`. The result must be called
// on `loop`'s thread (as HttpServer does), and so must the handler's
// responders.
RequestHandler limitConcurrency(EventLoop& loop, RequestHandler handler, AdmissionLimits limits);

class HttpConnection;   // private

class HttpServer {
public:
    HttpServer(EventLoop& loop, RequestHandler handler, HttpServerLimits limits = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Every connection from now on is TLS, with these certificates.
    void setTls(std::shared_ptr<ServerTls> tls) { tls_ = std::move(tls); }
    // The visitor's address for a request (a trusted proxy's forwarded one),
    // for the per-address stream cap. Default: the connection's peer.
    void setClientAddress(std::function<std::string(const HttpRequest&)> f) { clientAddress_ = std::move(f); }

    // Starts accepting connections on host:port (a numeric address; port 0
    // picks a free one, reported by port()). May be called more than once,
    // for more addresses.
    Status listen(const std::string& host, uint16_t port);
    // Starts accepting connections on a Unix-domain socket at `path`, made
    // with permission bits `mode` (see listenUnix). Its connections' peer
    // address is kUnixPeer, which is exempt from the per-address caps: such
    // a socket's clients are a local proxy (fourfold front).
    Status listenUnix(const std::string& path, unsigned mode = 0660);
    // Accepts connections on a socket that's already listening (from
    // systemd's socket activation). Takes it non-blocking.
    Status adopt(Fd listening);
    // The first listening socket's port.
    uint16_t port() const { return port_; }

    // Serves an already-connected, non-blocking socket. listen() uses this for
    // each connection it accepts; tests use it with a socket pair. Refused
    // (closed) past the connection caps.
    void addConnection(Fd fd, std::string peerAddress);

    // Graceful shutdown: stops accepting, closes idle connections, lets
    // requests in flight finish (ending streamed responses), and calls
    // `done` once every connection has closed, or when `deadline` has
    // passed (closing what's left).
    void shutdown(std::chrono::milliseconds deadline, std::function<void()> done);

    size_t connectionCount() const { return connections_.size(); }
    size_t streamCount() const { return streams_; }

private:
    friend class HttpConnection;
    friend class Http2Session;
    void onAcceptable(int fd);
    void removeConnection(HttpConnection* connection);
    bool exempt(const std::string& address) const;
    // Counts a stream for `address`, if it's within the caps.
    bool startStream(const std::string& address);
    void endStream(const std::string& address);
    void checkShutdown();

    EventLoop& loop_;
    RequestHandler handler_;
    HttpServerLimits limits_;
    std::shared_ptr<ServerTls> tls_;
    std::function<std::string(const HttpRequest&)> clientAddress_;
    std::vector<Fd> listeners_;
    uint16_t port_ = 0;
    std::unordered_map<HttpConnection*, std::shared_ptr<HttpConnection>> connections_;
    std::unordered_map<std::string, size_t> perAddress_;   // connections, by peer
    size_t streams_ = 0;
    std::unordered_map<std::string, size_t> streamsPerAddress_;
    bool shuttingDown_ = false;
    std::function<void()> shutdownDone_;
    EventLoop::TimerId shutdownTimer_ = 0;
};

}  // namespace fourfold

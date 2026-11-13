// One client connection of HttpServer: the socket, TLS if the server has it,
// and HTTP/1.1 (reading, parsing, writing); or, if ALPN chose it, HTTP/2
// through an Http2Session.
#pragma once

#include <llhttp.h>

#include <memory>
#include <string>

#include "httpserver/http_server.h"
#include "tls/server_tls.h"

namespace fourfold {

class Http2Session;

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    HttpConnection(HttpServer& server, Fd fd, std::string peerAddress);
    ~HttpConnection();

    // Starts watching the socket. Call once, right after construction.
    void start();
    // Stops watching and closes the socket, and removes this connection from
    // the server (which destroys it, once the caller lets go).
    void close();
    // Graceful shutdown: closes at once if idle; otherwise finishes the
    // request in flight (ending a streamed response) and then closes.
    void goAway();

    const std::string& peerAddress() const { return peerAddress_; }

private:
    friend class ConnectionStreamWriter;
    friend class Http2StreamWriter;
    friend class Http2Session;

    enum class State {
        Reading,     // waiting for (more of) a request
        Handling,    // a request is with the handler; not reading
        Writing,     // sending a response
        Draining,    // response sent, write side shut; discarding input until EOF
        Streaming,   // headers sent; the body is written as the stream provides it
        Http2,       // HTTP/2: the session handles requests and responses
        Closed,
    };

    void onIo(uint32_t events);
    void onReadable();
    // Takes bytes read from the socket: decrypts them if TLS, then hands
    // them on (parser, HTTP/2 session, or nowhere when draining). False if
    // the connection was closed.
    bool ingest(const char* data, size_t size, bool& peerClosed);
    void parseBuffered();
    void dispatch();
    void respond(HttpResponse response);
    void sendError(int status);
    void finishResponse();
    void onTimeout();
    // The bytes for the socket: encrypted first, if TLS.
    void queue(const std::string& bytes);
    void queue(const char* data, size_t size);
    // Writes what it can of outbuf_, then moves the connection on if it's
    // all gone (finishing a response, shutting the write side, ...).
    void flush();
    enum class WireState { Done, Blocked, Failed };
    WireState writeOut();
    void updateInterest();
    // Streamed responses.
    bool streamWrite(std::string bytes);
    void endStream();
    void releaseStream();
    void armTimer(std::chrono::milliseconds delay);
    void resetRequest();
    void startHttp2();

    // llhttp callbacks. Each returns 0 to continue, -1 to reject the request
    // (with errorStatus_ set), or HPE_PAUSED after a complete request.
    static int onMessageBegin(llhttp_t* p);
    static int onUrl(llhttp_t* p, const char* at, size_t length);
    static int onHeaderField(llhttp_t* p, const char* at, size_t length);
    static int onHeaderValue(llhttp_t* p, const char* at, size_t length);
    static int onHeaderValueComplete(llhttp_t* p);
    static int onHeadersComplete(llhttp_t* p);
    static int onBody(llhttp_t* p, const char* at, size_t length);
    static int onMessageComplete(llhttp_t* p);
    static HttpConnection& self(llhttp_t* p) { return *static_cast<HttpConnection*>(p->data); }
    int reject(int status);

    HttpServer& server_;
    EventLoop& loop_;
    const HttpServerLimits& limits_;
    size_t bodyLimit_ = 0;   // this request's: maxBodyBytes, or maxUploadBodyBytes for multipart
    Fd fd_;
    std::string peerAddress_;
    State state_ = State::Reading;
    uint32_t interest_ = 0;
    bool goingAway_ = false;     // graceful shutdown: no more requests
    bool shutWrite_ = false;     // the write side is shut (draining)

    std::unique_ptr<TlsSession> tls_;
    std::unique_ptr<Http2Session> h2_;

    llhttp_settings_t settings_;
    llhttp_t parser_;
    std::string inbuf_;          // received (decrypted), not yet parsed
    bool parsing_ = false;       // parseBuffered is running (it doesn't nest)
    bool reparse_ = false;       // parseBuffered was asked to run again
    std::string outbuf_;         // bytes for the socket, not yet sent
    size_t outSent_ = 0;

    // The request being parsed.
    HttpRequest request_;
    std::string headerName_, headerValue_;
    size_t headerBytes_ = 0;
    bool requestStarted_ = false;
    bool headersDone_ = false;
    std::chrono::steady_clock::time_point bodyStart_;
    size_t bodyBytes_ = 0;
    bool keepAlive_ = false;
    int errorStatus_ = 0;

    // The request being handled or answered.
    bool headRequest_ = false;
    bool http10_ = false;   // an HTTP/1.0 request: keep-alive must be said explicitly
    bool closeAfterWrite_ = false;
    std::string clientAddress_;   // the visitor's, for the stream caps

    // A streamed response: started once its headers are sent.
    std::function<void(std::shared_ptr<StreamWriter>)> pendingStream_;
    bool streamEnding_ = false;   // the stream closed; finish once the buffer is sent
    bool streamCounted_ = false;  // counted against the server's stream caps

    EventLoop::TimerId timer_ = 0;
};

}  // namespace fourfold

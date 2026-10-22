// One client connection of HttpServer: reading, parsing, and writing.
#pragma once

#include <llhttp.h>

#include <memory>
#include <string>

#include "httpserver/http_server.h"

namespace fourfold {

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    HttpConnection(HttpServer& server, Fd fd, std::string peerAddress);
    ~HttpConnection();

    // Starts watching the socket. Call once, right after construction.
    void start();
    // Stops watching and closes the socket, and removes this connection from
    // the server (which destroys it, once the caller lets go).
    void close();

private:
    enum class State {
        Reading,     // waiting for (more of) a request
        Handling,    // a request is with the handler; not reading
        Writing,     // sending a response
        Draining,    // response sent, write side shut; discarding input until EOF
        Closed,
    };

    void onIo(uint32_t events);
    void onReadable();
    void parseBuffered();
    void dispatch();
    void respond(HttpResponse response);
    void sendError(int status);
    void flush();
    void finishResponse();
    void onTimeout();
    void armTimer(std::chrono::milliseconds delay);
    void resetRequest();
    void setInterest(uint32_t interest);

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
    Fd fd_;
    std::string peerAddress_;
    State state_ = State::Reading;
    uint32_t interest_ = 0;

    llhttp_settings_t settings_;
    llhttp_t parser_;
    std::string inbuf_;          // received, not yet parsed
    bool parsing_ = false;       // parseBuffered is running (it doesn't nest)
    bool reparse_ = false;       // parseBuffered was asked to run again
    std::string outbuf_;         // serialized response, not yet fully sent
    size_t outSent_ = 0;

    // The request being parsed.
    HttpRequest request_;
    std::string headerName_, headerValue_;
    size_t headerBytes_ = 0;
    bool requestStarted_ = false;
    bool keepAlive_ = false;
    int errorStatus_ = 0;

    // The request being handled or answered.
    bool headRequest_ = false;
    bool closeAfterWrite_ = false;

    EventLoop::TimerId timer_ = 0;
};

}  // namespace fourfold

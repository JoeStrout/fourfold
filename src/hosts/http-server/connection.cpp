#include "connection.h"

#include <sys/socket.h>

namespace fourfold {

namespace {

constexpr size_t kReadChunk = 16 * 1024;
constexpr std::chrono::milliseconds kDrainTimeout{2000};

bool hasLineBreak(const std::string& s) {
    return s.find_first_of("\r\n") != std::string::npos;
}

// Headers the server writes itself, from the response's framing.
bool isFramingHeader(const std::string& name) {
    return equalsIgnoreCase(name, "Content-Length") || equalsIgnoreCase(name, "Connection") ||
           equalsIgnoreCase(name, "Transfer-Encoding");
}

}  // namespace

// The StreamWriter handed to a streamed response. It holds the connection
// weakly, so a stream outliving its client just finds itself closed.
class ConnectionStreamWriter : public StreamWriter {
public:
    explicit ConnectionStreamWriter(std::weak_ptr<HttpConnection> connection) : connection_(std::move(connection)) {}

    bool write(std::string bytes) override {
        auto c = connection_.lock();
        return c && c->streamWrite(std::move(bytes));
    }
    void close() override {
        if (auto c = connection_.lock()) c->endStream();
    }
    bool isOpen() const override {
        auto c = connection_.lock();
        return c && c->state_ == HttpConnection::State::Streaming && !c->streamEnding_;
    }

private:
    std::weak_ptr<HttpConnection> connection_;
};

HttpConnection::HttpConnection(HttpServer& server, Fd fd, std::string peerAddress)
    : server_(server),
      loop_(server.loop_),
      limits_(server.limits_),
      fd_(std::move(fd)),
      peerAddress_(std::move(peerAddress)) {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = onMessageBegin;
    settings_.on_url = onUrl;
    settings_.on_header_field = onHeaderField;
    settings_.on_header_value = onHeaderValue;
    settings_.on_header_value_complete = onHeaderValueComplete;
    settings_.on_headers_complete = onHeadersComplete;
    settings_.on_body = onBody;
    settings_.on_message_complete = onMessageComplete;
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

HttpConnection::~HttpConnection() {
    if (state_ != State::Closed) {
        loop_.cancelTimer(timer_);
        loop_.unwatch(fd_.get());
    }
}

void HttpConnection::start() {
    std::weak_ptr<HttpConnection> weak = shared_from_this();
    Status s = loop_.watch(fd_.get(), io::kReadable, [weak](uint32_t events) {
        if (auto c = weak.lock()) c->onIo(events);
    });
    if (!s) {
        close();
        return;
    }
    interest_ = io::kReadable;
    armTimer(limits_.idleTimeout);
}

void HttpConnection::close() {
    if (state_ == State::Closed) return;
    state_ = State::Closed;
    loop_.cancelTimer(timer_);
    loop_.unwatch(fd_.get());
    fd_.reset();
    server_.removeConnection(this);
}

void HttpConnection::setInterest(uint32_t interest) {
    if (interest == interest_) return;
    interest_ = interest;
    if (!loop_.setInterest(fd_.get(), interest)) close();
}

void HttpConnection::onIo(uint32_t events) {
    auto keep = shared_from_this();   // close() may drop the server's reference
    if ((events & io::kWritable) && (state_ == State::Writing || state_ == State::Streaming)) flush();
    if ((events & io::kReadable) && (state_ == State::Reading || state_ == State::Draining)) {
        onReadable();
    } else if ((events & io::kReadable) && state_ == State::Streaming) {
        discardInput();
    }
}

void HttpConnection::onReadable() {
    char buffer[kReadChunk];
    bool peerClosed = false;
    // Parse after each chunk, so at most one chunk waits while a request is
    // being handled.
    while (state_ == State::Reading || state_ == State::Draining) {
        auto n = readSome(fd_.get(), buffer, sizeof buffer);
        if (!n) {
            if (n.error().code == ErrorCode::WouldBlock) break;
            close();
            return;
        }
        if (n.value() == 0) {
            peerClosed = true;
            break;
        }
        if (state_ == State::Draining) continue;   // discard
        inbuf_.append(buffer, n.value());
        parseBuffered();
    }
    if (!peerClosed) return;
    if (state_ == State::Handling || state_ == State::Writing) {
        // The client finished sending; still answer the request it sent.
        closeAfterWrite_ = true;
    } else {
        close();
    }
}

void HttpConnection::parseBuffered() {
    if (parsing_) {
        reparse_ = true;
        return;
    }
    parsing_ = true;
    do {
        reparse_ = false;
        if (state_ != State::Reading || inbuf_.empty()) break;
        llhttp_errno_t err = llhttp_execute(&parser_, inbuf_.data(), inbuf_.size());
        if (err == HPE_OK) {
            inbuf_.clear();
        } else if (err == HPE_PAUSED) {
            // A complete request; anything after it is the next one.
            size_t used = static_cast<size_t>(llhttp_get_error_pos(&parser_) - inbuf_.data());
            inbuf_.erase(0, used);
            dispatch();   // if the handler answers at once, reparse_ is set
        } else {
            inbuf_.clear();
            sendError(errorStatus_ ? errorStatus_ : 400);
        }
    } while (reparse_);
    parsing_ = false;
}

void HttpConnection::dispatch() {
    state_ = State::Handling;
    loop_.cancelTimer(timer_);
    setInterest(0);
    if (state_ == State::Closed) return;

    HttpRequest request = std::move(request_);
    request.peerAddress = peerAddress_;
    headRequest_ = request.method == "HEAD";
    http10_ = request.versionMajor == 1 && request.versionMinor == 0;
    closeAfterWrite_ = closeAfterWrite_ || !keepAlive_;

    std::weak_ptr<HttpConnection> weak = shared_from_this();
    server_.handler_(std::move(request), [weak](HttpResponse response) {
        if (auto c = weak.lock()) c->respond(std::move(response));
    });
}

void HttpConnection::respond(HttpResponse response) {
    if (state_ != State::Handling) return;   // already answered, or closed

    const std::string* connection = response.headers.get("Connection");
    if (connection && equalsIgnoreCase(*connection, "close")) closeAfterWrite_ = true;

    int status = response.status;
    bool mayHaveBody = !(status < 200 || status == 204 || status == 304);
    bool streamed = response.stream && mayHaveBody && !headRequest_;
    if (streamed) {
        // The body ends when the connection does.
        closeAfterWrite_ = true;
        pendingStream_ = std::move(response.stream);
    }

    std::string& out = outbuf_;
    out = "HTTP/1.1 " + std::to_string(status) + " " + statusReason(status) + "\r\n";
    for (const Headers::Field& f : response.headers) {
        // Fields with line breaks would let a value inject headers.
        if (isFramingHeader(f.name) || hasLineBreak(f.name) || hasLineBreak(f.value)) continue;
        out += f.name + ": " + f.value + "\r\n";
    }
    if (mayHaveBody && !streamed) out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    if (closeAfterWrite_) {
        out += "Connection: close\r\n";
    } else if (http10_) {
        // HTTP/1.0 closes after each response unless told otherwise (RFC 9112 §9.3).
        out += "Connection: keep-alive\r\n";
    }
    out += "\r\n";
    if (mayHaveBody && !headRequest_ && !streamed) out += response.body;
    outSent_ = 0;

    state_ = State::Writing;
    armTimer(limits_.requestTimeout);   // a client that never reads is dropped
    flush();
}

void HttpConnection::sendError(int status) {
    state_ = State::Handling;
    headRequest_ = false;
    closeAfterWrite_ = true;
    respond(HttpResponse::text(status, std::string(statusReason(status)) + "\n"));
}

void HttpConnection::flush() {
    while (outSent_ < outbuf_.size()) {
        auto n = writeSome(fd_.get(), outbuf_.data() + outSent_, outbuf_.size() - outSent_);
        if (!n) {
            if (n.error().code == ErrorCode::WouldBlock) {
                setInterest(io::kWritable);
                return;
            }
            close();
            return;
        }
        outSent_ += n.value();
    }
    outbuf_.clear();
    outSent_ = 0;
    if (state_ == State::Streaming) {
        if (streamEnding_) {
            pendingStream_ = nullptr;
            state_ = State::Writing;
            finishResponse();   // closeAfterWrite_ is set: drains, then closes
        } else {
            setInterest(io::kReadable);   // to notice the client leaving
        }
        return;
    }
    finishResponse();
}

void HttpConnection::finishResponse() {
    if (pendingStream_) {
        // The headers are out; hand the stream its writer.
        state_ = State::Streaming;
        loop_.cancelTimer(timer_);
        timer_ = 0;
        setInterest(io::kReadable);
        if (state_ == State::Closed) return;
        auto start = std::move(pendingStream_);
        pendingStream_ = nullptr;
        auto keep = shared_from_this();
        start(std::make_shared<ConnectionStreamWriter>(weak_from_this()));
        return;
    }
    if (closeAfterWrite_) {
        // Shut our side and discard what the client still sends, so closing
        // doesn't reset the connection before the client reads the response.
        state_ = State::Draining;
        shutdown(fd_.get(), SHUT_WR);
        inbuf_.clear();
        setInterest(io::kReadable);
        armTimer(kDrainTimeout);
        return;
    }
    state_ = State::Reading;
    resetRequest();
    llhttp_resume(&parser_);
    setInterest(io::kReadable);
    armTimer(limits_.idleTimeout);
    parseBuffered();   // a pipelined request may already be waiting
}

bool HttpConnection::streamWrite(std::string bytes) {
    if (state_ != State::Streaming || streamEnding_) return false;
    if (outbuf_.size() - outSent_ + bytes.size() > limits_.maxStreamBufferBytes) {
        close();   // the client isn't reading
        return false;
    }
    outbuf_ += bytes;
    flush();
    return state_ == State::Streaming;
}

void HttpConnection::endStream() {
    if (state_ != State::Streaming || streamEnding_) return;
    streamEnding_ = true;
    flush();   // finishes at once if nothing is waiting
}

void HttpConnection::discardInput() {
    char buffer[kReadChunk];
    for (;;) {
        auto n = readSome(fd_.get(), buffer, sizeof buffer);
        if (!n) {
            if (n.error().code == ErrorCode::WouldBlock) return;
            close();
            return;
        }
        if (n.value() == 0) {
            close();   // the client has gone
            return;
        }
    }
}

void HttpConnection::armTimer(std::chrono::milliseconds delay) {
    loop_.cancelTimer(timer_);
    std::weak_ptr<HttpConnection> weak = shared_from_this();
    timer_ = loop_.addTimer(delay, [weak] {
        if (auto c = weak.lock()) {
            c->timer_ = 0;
            c->onTimeout();
        }
    });
}

void HttpConnection::onTimeout() {
    if (state_ == State::Reading && requestStarted_) {
        sendError(408);
    } else {
        close();
    }
}

void HttpConnection::resetRequest() {
    request_ = HttpRequest{};
    headerName_.clear();
    headerValue_.clear();
    headerBytes_ = 0;
    requestStarted_ = false;
    keepAlive_ = false;
    errorStatus_ = 0;
    headRequest_ = false;
}

int HttpConnection::reject(int status) {
    errorStatus_ = status;
    return -1;
}

int HttpConnection::onMessageBegin(llhttp_t* p) {
    HttpConnection& c = self(p);
    c.requestStarted_ = true;
    c.armTimer(c.limits_.requestTimeout);
    return 0;
}

int HttpConnection::onUrl(llhttp_t* p, const char* at, size_t length) {
    HttpConnection& c = self(p);
    c.headerBytes_ += length;
    if (c.headerBytes_ > c.limits_.maxHeaderBytes) return c.reject(414);
    c.request_.target.append(at, length);
    return 0;
}

int HttpConnection::onHeaderField(llhttp_t* p, const char* at, size_t length) {
    HttpConnection& c = self(p);
    c.headerBytes_ += length;
    if (c.headerBytes_ > c.limits_.maxHeaderBytes) return c.reject(431);
    c.headerName_.append(at, length);
    return 0;
}

int HttpConnection::onHeaderValue(llhttp_t* p, const char* at, size_t length) {
    HttpConnection& c = self(p);
    c.headerBytes_ += length;
    if (c.headerBytes_ > c.limits_.maxHeaderBytes) return c.reject(431);
    c.headerValue_.append(at, length);
    return 0;
}

int HttpConnection::onHeaderValueComplete(llhttp_t* p) {
    HttpConnection& c = self(p);
    if (c.request_.headers.size() >= c.limits_.maxHeaderCount) return c.reject(431);
    c.request_.headers.add(std::move(c.headerName_), std::move(c.headerValue_));
    c.headerName_.clear();
    c.headerValue_.clear();
    return 0;
}

int HttpConnection::onHeadersComplete(llhttp_t* p) {
    HttpConnection& c = self(p);
    c.request_.method = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(p)));
    c.request_.versionMajor = llhttp_get_http_major(p);
    c.request_.versionMinor = llhttp_get_http_minor(p);
    if ((p->flags & F_CONTENT_LENGTH) && p->content_length > c.limits_.maxBodyBytes) {
        return c.reject(413);
    }
    const std::string* expect = c.request_.headers.get("Expect");
    if (expect && equalsIgnoreCase(*expect, "100-continue")) {
        // Best effort: if it doesn't fit in the socket buffer, the client
        // sends the body anyway after a short wait.
        static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
        (void)writeSome(c.fd_.get(), kContinue, sizeof kContinue - 1);
    }
    return 0;
}

int HttpConnection::onBody(llhttp_t* p, const char* at, size_t length) {
    HttpConnection& c = self(p);
    if (c.request_.body.size() + length > c.limits_.maxBodyBytes) return c.reject(413);
    c.request_.body.append(at, length);
    return 0;
}

int HttpConnection::onMessageComplete(llhttp_t* p) {
    HttpConnection& c = self(p);
    c.keepAlive_ = llhttp_should_keep_alive(p) && !p->upgrade;
    return HPE_PAUSED;
}

}  // namespace fourfold

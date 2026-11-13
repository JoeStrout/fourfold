#include "connection.h"

#include <sys/socket.h>

#include <algorithm>

#include "http2.h"

namespace fourfold {

namespace {

constexpr size_t kReadChunk = 16 * 1024;
constexpr std::chrono::milliseconds kDrainTimeout{2000};
constexpr std::chrono::milliseconds kBodyCheckInterval{1000};

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
    if (server.tls_) tls_ = std::make_unique<TlsSession>(server.tls_);
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
    // The first request (and the TLS handshake before it) must come
    // promptly: a connection that sends nothing is a slowloris too.
    armTimer(limits_.headerTimeout);
}

void HttpConnection::close() {
    if (state_ == State::Closed) return;
    auto keep = shared_from_this();
    state_ = State::Closed;
    loop_.cancelTimer(timer_);
    loop_.unwatch(fd_.get());
    fd_.reset();
    releaseStream();
    if (h2_) h2_->closed();
    server_.removeConnection(this);
}

void HttpConnection::goAway() {
    if (state_ == State::Closed || goingAway_) return;
    goingAway_ = true;
    switch (state_) {
        case State::Reading:
            if (!requestStarted_) {
                close();
            } else {
                closeAfterWrite_ = true;   // answer it, then close
            }
            break;
        case State::Handling:
        case State::Writing:
            closeAfterWrite_ = true;
            break;
        case State::Streaming:
            endStream();
            break;
        case State::Http2:
            h2_->goAway();
            flush();
            break;
        case State::Draining:
        case State::Closed:
            break;
    }
}

void HttpConnection::updateInterest() {
    if (state_ == State::Closed) return;
    bool pending = outSent_ < outbuf_.size();
    uint32_t interest = pending ? io::kWritable : 0;
    switch (state_) {
        case State::Reading:
        case State::Streaming:
        case State::Draining:
        case State::Http2:
            interest |= io::kReadable;
            break;
        case State::Writing:
            interest = io::kWritable;
            break;
        case State::Handling:
        case State::Closed:
            break;
    }
    if (interest == interest_) return;
    interest_ = interest;
    if (!loop_.setInterest(fd_.get(), interest)) close();
}

void HttpConnection::onIo(uint32_t events) {
    auto keep = shared_from_this();   // close() may drop the server's reference
    if ((events & io::kWritable) && outSent_ < outbuf_.size()) flush();
    if (state_ == State::Closed) return;
    if (events & io::kReadable) onReadable();
}

void HttpConnection::onReadable() {
    char buffer[kReadChunk];
    bool peerClosed = false;
    // Parse after each chunk, so at most one chunk waits while a request is
    // being handled.
    while (state_ == State::Reading || state_ == State::Draining || state_ == State::Streaming ||
           state_ == State::Http2) {
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
        if (!ingest(buffer, n.value(), peerClosed)) return;
        if (peerClosed) break;
    }
    if (!peerClosed) return;
    if (state_ == State::Handling || state_ == State::Writing) {
        // The client finished sending; still answer the request it sent.
        closeAfterWrite_ = true;
    } else {
        close();
    }
}

bool HttpConnection::ingest(const char* data, size_t size, bool& peerClosed) {
    std::string decrypted;
    const std::string* plain = nullptr;
    std::string direct;
    if (tls_) {
        TlsSession::State st = tls_->receive(data, size, decrypted);
        if (tls_->hasOutput()) tls_->takeOutput(outbuf_);
        if (st == TlsSession::State::Failed) {
            (void)writeOut();   // the alert, if it fits
            close();
            return false;
        }
        if (st == TlsSession::State::Closed) peerClosed = true;
        if (!h2_ && tls_->handshakeDone() && tls_->protocol() == "h2" && state_ == State::Reading &&
            !requestStarted_ && inbuf_.empty()) {
            startHttp2();
            if (state_ == State::Closed) return false;
        }
        plain = &decrypted;
    } else {
        direct.assign(data, size);
        plain = &direct;
    }
    if (outSent_ < outbuf_.size()) {
        flush();
        if (state_ == State::Closed) return false;
    }
    if (plain->empty()) return true;
    switch (state_) {
        case State::Reading:
            inbuf_ += *plain;
            parseBuffered();
            break;
        case State::Http2:
            if (!h2_->receive(*plain)) {
                flush();   // the GOAWAY
                if (state_ != State::Closed) close();
                return false;
            }
            flush();
            break;
        default:
            break;   // draining or streaming: input is discarded
    }
    return state_ != State::Closed;
}

void HttpConnection::startHttp2() {
    h2_ = std::make_unique<Http2Session>(*this);
    if (!h2_->start()) {
        close();
        return;
    }
    state_ = State::Http2;
    armTimer(limits_.idleTimeout);
    flush();
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
    timer_ = 0;
    updateInterest();
    if (state_ == State::Closed) return;

    HttpRequest request = std::move(request_);
    request.peerAddress = peerAddress_;
    headRequest_ = request.method == "HEAD";
    http10_ = request.versionMajor == 1 && request.versionMinor == 0;
    closeAfterWrite_ = closeAfterWrite_ || !keepAlive_ || goingAway_;
    clientAddress_ = server_.clientAddress_ ? server_.clientAddress_(request) : peerAddress_;

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
    if (streamed && !server_.startStream(clientAddress_)) {
        // Too many open streams: the client (HTMX's SSE extension, an
        // EventSource) retries later.
        response = HttpResponse::text(503, "Service Unavailable: too many open event streams\n");
        response.headers.set("Retry-After", "10");
        status = 503;
        streamed = false;
    } else if (streamed) {
        streamCounted_ = true;
    }
    if (streamed) {
        // The body ends when the connection does.
        closeAfterWrite_ = true;
        pendingStream_ = std::move(response.stream);
    }

    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + statusReason(status) + "\r\n";
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
    queue(out);

    state_ = State::Writing;
    armTimer(limits_.sendTimeout);   // a client that never reads is dropped
    flush();
}

void HttpConnection::sendError(int status) {
    state_ = State::Handling;
    headRequest_ = false;
    closeAfterWrite_ = true;
    respond(HttpResponse::text(status, std::string(statusReason(status)) + "\n"));
}

void HttpConnection::queue(const std::string& bytes) {
    queue(bytes.data(), bytes.size());
}

void HttpConnection::queue(const char* data, size_t size) {
    if (outSent_ > 0 && outSent_ == outbuf_.size()) {
        outbuf_.clear();
        outSent_ = 0;
    }
    if (tls_) {
        tls_->send(data, size);
        tls_->takeOutput(outbuf_);
    } else {
        outbuf_.append(data, size);
    }
}

HttpConnection::WireState HttpConnection::writeOut() {
    size_t before = outSent_;
    while (outSent_ < outbuf_.size()) {
        auto n = writeSome(fd_.get(), outbuf_.data() + outSent_, outbuf_.size() - outSent_);
        if (!n) {
            if (n.error().code == ErrorCode::WouldBlock) break;
            return WireState::Failed;
        }
        outSent_ += n.value();
    }
    // Progress restarts the clock on a client that's slow to read.
    if (outSent_ > before && state_ == State::Writing) armTimer(limits_.sendTimeout);
    if (outSent_ < outbuf_.size()) {
        // Don't let the sent part grow without bound.
        if (outSent_ > 64 * 1024) {
            outbuf_.erase(0, outSent_);
            outSent_ = 0;
        }
        return WireState::Blocked;
    }
    outbuf_.clear();
    outSent_ = 0;
    return WireState::Done;
}

void HttpConnection::flush() {
    if (state_ == State::Closed) return;
    auto keep = shared_from_this();
    for (;;) {
        WireState w = writeOut();
        if (w == WireState::Failed) {
            close();
            return;
        }
        if (w == WireState::Blocked) {
            updateInterest();
            return;
        }
        // HTTP/2 makes its output as there's room for it.
        if (state_ == State::Http2 && h2_->produce()) continue;
        break;
    }
    switch (state_) {
        case State::Writing:
            finishResponse();
            return;
        case State::Streaming:
            if (streamEnding_) {
                pendingStream_ = nullptr;
                state_ = State::Writing;
                finishResponse();   // closeAfterWrite_ is set: drains, then closes
                return;
            }
            break;
        case State::Draining:
            if (!shutWrite_) {
                shutWrite_ = true;
                ::shutdown(fd_.get(), SHUT_WR);
            }
            break;
        case State::Http2:
            if (h2_->finished()) {
                close();
                return;
            }
            if (h2_->idle() && goingAway_) {
                close();
                return;
            }
            if (!timer_) armTimer(h2_->idle() ? limits_.idleTimeout : kBodyCheckInterval);
            break;
        default:
            break;
    }
    updateInterest();
}

void HttpConnection::finishResponse() {
    if (pendingStream_) {
        // The headers are out; hand the stream its writer.
        state_ = State::Streaming;
        loop_.cancelTimer(timer_);
        timer_ = 0;
        updateInterest();
        if (state_ == State::Closed) return;
        auto start = std::move(pendingStream_);
        pendingStream_ = nullptr;
        auto keep = shared_from_this();
        start(std::make_shared<ConnectionStreamWriter>(weak_from_this()));
        if (goingAway_ && state_ == State::Streaming) endStream();
        return;
    }
    releaseStream();
    if (closeAfterWrite_) {
        // Shut our side and discard what the client still sends, so closing
        // doesn't reset the connection before the client reads the response.
        state_ = State::Draining;
        inbuf_.clear();
        if (tls_) {
            tls_->close();   // close_notify
            tls_->takeOutput(outbuf_);
        }
        armTimer(kDrainTimeout);
        flush();
        return;
    }
    state_ = State::Reading;
    resetRequest();
    llhttp_resume(&parser_);
    updateInterest();
    armTimer(limits_.idleTimeout);
    parseBuffered();   // a pipelined request may already be waiting
}

bool HttpConnection::streamWrite(std::string bytes) {
    if (state_ != State::Streaming || streamEnding_) return false;
    if (outbuf_.size() - outSent_ + bytes.size() > limits_.maxStreamBufferBytes) {
        close();   // the client isn't reading
        return false;
    }
    queue(bytes);
    flush();
    return state_ == State::Streaming;
}

void HttpConnection::endStream() {
    if (state_ != State::Streaming || streamEnding_) return;
    streamEnding_ = true;
    flush();   // finishes at once if nothing is waiting
}

void HttpConnection::releaseStream() {
    if (!streamCounted_) return;
    streamCounted_ = false;
    server_.endStream(clientAddress_);
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
    auto keep = shared_from_this();
    switch (state_) {
        case State::Reading:
            if (!requestStarted_) {
                close();   // idle, or never sent anything
            } else if (!headersDone_) {
                sendError(408);   // headers too slow
            } else {
                // The body: is it arriving fast enough, on average?
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - bodyStart_);
                double expected = static_cast<double>(limits_.minBodyBytesPerSecond) *
                                  static_cast<double>((elapsed - limits_.bodyGrace).count()) / 1000.0;
                if (static_cast<double>(bodyBytes_) < expected) {
                    sendError(408);
                } else {
                    armTimer(std::min(kBodyCheckInterval, limits_.bodyGrace));
                }
            }
            break;
        case State::Http2:
            if (h2_->idle()) {
                auto idleFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - h2_->idleSince());
                if (idleFor >= limits_.idleTimeout) {
                    h2_->goAway();
                    goingAway_ = true;
                } else {
                    armTimer(limits_.idleTimeout - idleFor);
                }
            } else {
                h2_->checkTimers();
            }
            flush();   // arms the next check, if none is armed
            break;
        default:
            close();   // not reading the response, or not finishing the drain
            break;
    }
}

void HttpConnection::resetRequest() {
    request_ = HttpRequest{};
    headerName_.clear();
    headerValue_.clear();
    headerBytes_ = 0;
    requestStarted_ = false;
    headersDone_ = false;
    bodyBytes_ = 0;
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
    c.armTimer(c.limits_.headerTimeout);
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
    c.bodyLimit_ = c.limits_.maxBodyBytes;
    if (const std::string* type = c.request_.headers.get("Content-Type")) {
        std::string media = type->substr(0, type->find(';'));
        while (!media.empty() && media.back() == ' ') media.pop_back();
        if (equalsIgnoreCase(media, "multipart/form-data")) c.bodyLimit_ = c.limits_.maxUploadBodyBytes;
    }
    if ((p->flags & F_CONTENT_LENGTH) && p->content_length > c.bodyLimit_) {
        return c.reject(413);
    }
    // The headers are in; from here, the body is held to a minimum rate.
    c.headersDone_ = true;
    c.bodyStart_ = std::chrono::steady_clock::now();
    c.bodyBytes_ = 0;
    c.armTimer(c.limits_.bodyGrace);
    const std::string* expect = c.request_.headers.get("Expect");
    if (expect && equalsIgnoreCase(*expect, "100-continue")) {
        c.queue("HTTP/1.1 100 Continue\r\n\r\n");
        (void)c.writeOut();   // the rest, if any, goes with the next flush
    }
    return 0;
}

int HttpConnection::onBody(llhttp_t* p, const char* at, size_t length) {
    HttpConnection& c = self(p);
    if (c.request_.body.size() + length > c.bodyLimit_) return c.reject(413);
    c.request_.body.append(at, length);
    c.bodyBytes_ += length;
    return 0;
}

int HttpConnection::onMessageComplete(llhttp_t* p) {
    HttpConnection& c = self(p);
    c.keepAlive_ = llhttp_should_keep_alive(p) && !p->upgrade;
    return HPE_PAUSED;
}

}  // namespace fourfold

#include "front/upstream.h"

#include <algorithm>
#include <cctype>

#include <llhttp.h>

#include "platform/address.h"
#include "platform/socket.h"

namespace fourfold {

namespace {

// Hop-by-hop fields (RFC 9110 §7.6.1), and the ones front writes itself.
bool isHopByHop(const std::string& name) {
    static const char* const kNames[] = {"Connection", "Keep-Alive",       "Proxy-Connection", "TE",
                                         "Trailer",    "Transfer-Encoding", "Upgrade",          "Content-Length"};
    for (const char* n : kNames) {
        if (equalsIgnoreCase(name, n)) return true;
    }
    return false;
}

// What a visitor may not tell the engine: who it is, as a proxy would.
bool isForwarding(const std::string& name) {
    static const char* const kNames[] = {"Forwarded", "X-Forwarded-For", "X-Forwarded-Proto", "X-Forwarded-Host",
                                         "X-Real-IP", "Expect"};
    for (const char* n : kNames) {
        if (equalsIgnoreCase(name, n)) return true;
    }
    return false;
}

bool hasLineBreak(const std::string& s) { return s.find_first_of("\r\n") != std::string::npos; }

// The names a Connection field lists, which are hop-by-hop too.
std::vector<std::string> connectionOptions(const Headers& headers) {
    std::vector<std::string> out;
    for (const Headers::Field& f : headers) {
        if (!equalsIgnoreCase(f.name, "Connection")) continue;
        size_t start = 0;
        while (start <= f.value.size()) {
            size_t comma = f.value.find(',', start);
            if (comma == std::string::npos) comma = f.value.size();
            std::string token = f.value.substr(start, comma - start);
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            if (!token.empty()) out.push_back(token);
            start = comma + 1;
        }
    }
    return out;
}

bool listed(const std::vector<std::string>& names, const std::string& name) {
    for (const std::string& n : names) {
        if (equalsIgnoreCase(n, name)) return true;
    }
    return false;
}

HttpResponse frontError(int status, const std::string& message) {
    HttpResponse r = HttpResponse::text(status, std::string(statusReason(status)) + ": " + message + "\n");
    r.headers.set("Cache-Control", "no-store");
    if (status == 503) r.headers.set("Retry-After", "10");
    return r;
}

}  // namespace

std::string forwardedElement(const std::string& visitor, const std::string& proto, const std::string& host) {
    std::string v = visitor;
    if (v.compare(0, 7, "::ffff:") == 0 && v.find('.') != std::string::npos) v = v.substr(7);
    std::string out = v.find(':') != std::string::npos ? "for=\"[" + v + "]\"" : "for=" + v;
    if (!proto.empty()) out += ";proto=" + proto;
    if (!host.empty()) {
        bool token = std::all_of(host.begin(), host.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
        });
        // A host:port, or anything odd, goes in quotes (with no quotes or
        // backslashes of its own).
        if (token) {
            out += ";host=" + host;
        } else if (host.find_first_of("\"\\") == std::string::npos) {
            out += ";host=\"" + host + "\"";
        }
    }
    return out;
}

std::string upstreamRequest(const HttpRequest& request, const std::string& visitor, const std::string& proto) {
    std::string method = request.method == "HEAD" ? "GET" : request.method;
    std::string out = method + " " + request.target + " HTTP/1.1\r\n";
    std::vector<std::string> options = connectionOptions(request.headers);
    for (const Headers::Field& f : request.headers) {
        if (isHopByHop(f.name) || isForwarding(f.name) || listed(options, f.name)) continue;
        if (f.name.empty() || hasLineBreak(f.name) || hasLineBreak(f.value)) continue;
        out += f.name + ": " + f.value + "\r\n";
    }
    const std::string* host = request.headers.get("Host");
    out += "Forwarded: " + forwardedElement(visitor, proto, host ? *host : std::string()) + "\r\n";
    bool bodyMethod = method == "POST" || method == "PUT" || method == "PATCH";
    if (!request.body.empty() || bodyMethod) out += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
    out += "\r\n";
    out += request.body;
    return out;
}

// --- One connection to the engine ---

// One connection to the engine, carrying one exchange at a time.
class UpstreamConnection : public std::enable_shared_from_this<UpstreamConnection> {
public:
    UpstreamConnection(Upstream& owner, Fd fd) : owner_(&owner), loop_(owner.loop_), fd_(std::move(fd)) {
        llhttp_settings_init(&settings_);
        settings_.on_header_field = onHeaderField;
        settings_.on_header_value = onHeaderValue;
        settings_.on_headers_complete = onHeadersComplete;
        settings_.on_body = onBody;
        settings_.on_message_complete = onMessageComplete;
        llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
        parser_.data = this;
    }

    ~UpstreamConnection() { stopIo(); }

    Status start() {
        std::weak_ptr<UpstreamConnection> weak = shared_from_this();
        return loop_.watch(fd_.get(), io::kReadable, [weak](uint32_t events) {
            if (auto c = weak.lock()) c->onIo(events);
        });
    }

    // Sends one request; `retryable`: on a reused connection, which the
    // engine may have closed just now, so a failure before any answer is
    // worth one more try on a fresh one.
    void send(std::string wire, Responder respond, bool retryable) {
        loop_.cancelTimer(timer_);
        llhttp_reset(&parser_);
        wire_ = std::move(wire);
        out_ = wire_;
        respond_ = std::move(respond);
        retryable_ = retryable;
        received_ = false;
        delivered_ = false;
        complete_ = false;
        streaming_ = false;
        response_ = HttpResponse{};
        fields_.clear();
        headerBytes_ = 0;
        lastWasValue_ = false;
        std::weak_ptr<UpstreamConnection> weak = shared_from_this();
        timer_ = loop_.addTimer(owner_->limits_.responseTimeout, [weak] {
            if (auto c = weak.lock()) {
                c->timer_ = 0;
                c->fail(504, "the site took too long to answer");
            }
        });
        flush();
    }

    // Kept for reuse: closed if the engine closes it, or after a while.
    void park() {
        loop_.cancelTimer(timer_);
        respond_ = nullptr;
        std::weak_ptr<UpstreamConnection> weak = shared_from_this();
        timer_ = loop_.addTimer(owner_->limits_.idleTimeout, [weak] {
            if (auto c = weak.lock()) {
                c->timer_ = 0;
                c->close();
            }
        });
    }

    // Closes the connection; a response still owed is a 502.
    void close() {
        if (closed_) return;
        auto keep = shared_from_this();
        closed_ = true;
        if (respond_ && !delivered_) deliver(frontError(502, "the site closed the connection"));
        if (writer_) writer_->close();
        writer_.reset();
        stopIo();
        if (owner_) owner_->forget(this);
        selfHold_.reset();
    }

    void detach() { owner_ = nullptr; }

private:
    void stopIo() {
        loop_.cancelTimer(timer_);
        timer_ = 0;
        loop_.cancelTimer(checkTimer_);
        checkTimer_ = 0;
        if (fd_.valid()) {
            loop_.unwatch(fd_.get());
            fd_.reset();
        }
    }

    void deliver(HttpResponse r) {
        delivered_ = true;
        Responder respond = std::move(respond_);
        respond_ = nullptr;
        if (respond) respond(std::move(r));
    }

    void fail(int status, const std::string& why) {
        if (closed_) return;
        auto keep = shared_from_this();
        if (!delivered_ && !received_ && retryable_ && owner_ && respond_) {
            // A kept connection the engine had closed: once more, afresh.
            Responder respond = std::move(respond_);
            respond_ = nullptr;
            Upstream* owner = owner_;
            std::string wire = std::move(wire_);
            close();
            owner->sendOn(nullptr, std::move(wire), std::move(respond), false);
            return;
        }
        if (!delivered_ && respond_) deliver(frontError(status, why));
        close();
    }

    void flush() {
        while (!out_.empty()) {
            auto n = writeSome(fd_.get(), out_.data(), out_.size());
            if (!n) {
                if (n.error().code == ErrorCode::WouldBlock) break;
                fail(502, "the site closed the connection");
                return;
            }
            out_.erase(0, n.value());
        }
        (void)loop_.setInterest(fd_.get(), out_.empty() ? io::kReadable : io::kReadable | io::kWritable);
    }

    void onIo(uint32_t events) {
        auto keep = shared_from_this();
        if ((events & io::kWritable) && !closed_) flush();
        if (!(events & io::kReadable) || closed_) return;
        char buffer[65536];
        for (int round = 0; round < 16 && !closed_; round++) {
            auto n = readSome(fd_.get(), buffer, sizeof buffer);
            if (!n) {
                if (n.error().code == ErrorCode::WouldBlock) return;
                fail(502, "the site closed the connection");
                return;
            }
            if (n.value() == 0) {
                ended();
                return;
            }
            if (!respond_ && !streaming_) {
                // Bytes while idle: nothing is owed; the connection is no good.
                close();
                return;
            }
            received_ = true;
            llhttp_errno_t e = llhttp_execute(&parser_, buffer, n.value());
            if (e == HPE_PAUSED) {
                // The response is complete (onMessageComplete paused).
                finished();
                return;
            }
            if (e != HPE_OK) {
                fail(502, "the site's response was malformed");
                return;
            }
        }
    }

    // The engine closed its end.
    void ended() {
        if (respond_ || streaming_) {
            llhttp_errno_t e = llhttp_finish(&parser_);
            if ((e == HPE_OK || e == HPE_PAUSED) && complete_) {
                finished();
                return;
            }
        }
        fail(502, "the site closed the connection");
    }

    void finished() {
        auto keep = shared_from_this();
        loop_.cancelTimer(timer_);
        timer_ = 0;
        if (streaming_) {
            streamEnded_ = true;
            if (writerSeen_) {
                close();   // ends the visitor's stream too
                return;
            }
            // The visitor's server hasn't asked for the body yet: the
            // connection goes, but this stays (with what's pending) until it
            // does, or checkVisitor gives up.
            loop_.unwatch(fd_.get());
            fd_.reset();
            selfHold_ = shared_from_this();
            if (owner_) owner_->forget(this);
            owner_ = nullptr;
            return;
        }
        bool reusable = llhttp_should_keep_alive(&parser_) && out_.empty();
        deliver(std::move(response_));
        if (owner_) owner_->release(this, reusable);
    }

    // --- Parsing ---

    static UpstreamConnection* of(llhttp_t* p) { return static_cast<UpstreamConnection*>(p->data); }

    static int onHeaderField(llhttp_t* p, const char* at, size_t length) {
        UpstreamConnection* c = of(p);
        if ((c->headerBytes_ += length) > c->owner_->limits_.maxHeaderBytes) return -1;
        if (c->lastWasValue_ || c->fields_.empty()) c->fields_.push_back({});
        c->fields_.back().name.append(at, length);
        c->lastWasValue_ = false;
        return 0;
    }

    static int onHeaderValue(llhttp_t* p, const char* at, size_t length) {
        UpstreamConnection* c = of(p);
        if ((c->headerBytes_ += length) > c->owner_->limits_.maxHeaderBytes) return -1;
        if (c->fields_.empty()) return -1;
        c->fields_.back().value.append(at, length);
        c->lastWasValue_ = true;
        return 0;
    }

    static int onHeadersComplete(llhttp_t* p) {
        UpstreamConnection* c = of(p);
        int status = llhttp_get_status_code(p);
        if (status < 200) return -1;   // no 1xx: front sends no Expect
        c->response_.status = status;
        std::vector<std::string> options;
        for (const Headers::Field& f : c->fields_) {
            if (equalsIgnoreCase(f.name, "Connection")) {
                Headers h;
                h.add(f.name, f.value);
                for (const std::string& o : connectionOptions(h)) options.push_back(o);
            }
        }
        bool hasLength = false;
        for (const Headers::Field& f : c->fields_) {
            if (equalsIgnoreCase(f.name, "Content-Length")) hasLength = true;
            if (isHopByHop(f.name) || listed(options, f.name)) continue;
            c->response_.headers.add(f.name, f.value);
        }
        bool noBody = status == 204 || status == 304;
        bool chunked = (p->flags & F_CHUNKED) != 0;
        if (noBody || (hasLength && !chunked && p->content_length <= c->owner_->limits_.maxBufferedBody)) return 0;
        c->startStream();
        return 0;
    }

    static int onBody(llhttp_t* p, const char* at, size_t length) {
        UpstreamConnection* c = of(p);
        if (!c->streaming_) {
            c->response_.body.append(at, length);
            return 0;
        }
        return c->streamBody(std::string(at, length)) ? 0 : -1;
    }

    static int onMessageComplete(llhttp_t* p) {
        of(p)->complete_ = true;
        return HPE_PAUSED;
    }

    // --- Streaming ---

    void startStream() {
        streaming_ = true;
        loop_.cancelTimer(timer_);
        timer_ = 0;
        std::weak_ptr<UpstreamConnection> weak = shared_from_this();
        HttpResponse r = std::move(response_);
        response_ = HttpResponse{};
        r.stream = [weak](std::shared_ptr<StreamWriter> writer) {
            if (auto c = weak.lock()) {
                c->attach(std::move(writer));
            } else {
                writer->close();
            }
        };
        // If the visitor has gone before the body could start, their server
        // never asks: give up after a while.
        checkTimer_ = loop_.addTimer(std::chrono::seconds(15), [weak] {
            if (auto c = weak.lock()) c->checkVisitor();
        });
        deliver(std::move(r));
    }

    void attach(std::shared_ptr<StreamWriter> writer) {
        auto keep = shared_from_this();
        writerSeen_ = true;
        if (!pending_.empty()) {
            std::string first = std::move(pending_);
            pending_.clear();
            if (!writer->write(std::move(first))) {
                close();
                return;
            }
        }
        if (streamEnded_ || closed_) {
            writer->close();
            close();
            return;
        }
        writer_ = std::move(writer);
    }

    // Visitor gone, or not keeping up: false (and the engine's end closes).
    bool streamBody(std::string bytes) {
        if (writer_) {
            if (writer_->write(std::move(bytes))) return true;
            writer_.reset();
            return false;
        }
        pending_ += bytes;
        return pending_.size() <= owner_->limits_.maxBufferedBody;
    }

    // Every 15 seconds while streaming: a visitor who has gone is noticed
    // even if the engine has nothing to send.
    void checkVisitor() {
        checkTimer_ = 0;
        if (closed_) return;
        if ((writerSeen_ && (!writer_ || !writer_->isOpen())) || (!writerSeen_ && ++unseenChecks_ >= 2)) {
            close();
            return;
        }
        std::weak_ptr<UpstreamConnection> weak = shared_from_this();
        checkTimer_ = loop_.addTimer(std::chrono::seconds(15), [weak] {
            if (auto c = weak.lock()) c->checkVisitor();
        });
    }

    Upstream* owner_;
    EventLoop& loop_;
    Fd fd_;
    llhttp_t parser_;
    llhttp_settings_t settings_;
    std::string wire_;   // the request, for a retry
    std::string out_;
    Responder respond_;
    bool retryable_ = false;
    bool received_ = false;
    bool delivered_ = false;
    bool complete_ = false;
    bool closed_ = false;
    HttpResponse response_;
    std::vector<Headers::Field> fields_;
    size_t headerBytes_ = 0;
    bool lastWasValue_ = false;
    EventLoop::TimerId timer_ = 0;
    // Streaming:
    bool streaming_ = false;
    bool streamEnded_ = false;
    bool writerSeen_ = false;
    int unseenChecks_ = 0;
    std::string pending_;
    std::shared_ptr<StreamWriter> writer_;
    EventLoop::TimerId checkTimer_ = 0;
    // Keeps an ended stream's pending body until the visitor's server asks.
    std::shared_ptr<UpstreamConnection> selfHold_;
};

// --- Upstream ---

std::shared_ptr<Upstream> Upstream::create(EventLoop& loop, std::string name, std::string socketPath,
                                           UpstreamLimits limits) {
    return std::shared_ptr<Upstream>(new Upstream(loop, std::move(name), std::move(socketPath), limits));
}

Upstream::~Upstream() { closeAll(); }

void Upstream::forward(HttpRequest request, const std::string& visitor, const std::string& proto, Responder respond) {
    std::string wire = upstreamRequest(request, visitor, proto);
    std::shared_ptr<UpstreamConnection> reuse;
    if (!idle_.empty()) {
        reuse = std::move(idle_.back());
        idle_.pop_back();
    }
    sendOn(std::move(reuse), std::move(wire), std::move(respond), true);
}

void Upstream::sendOn(std::shared_ptr<UpstreamConnection> c, std::string wire, Responder respond, bool retryable) {
    if (!c) {
        retryable = false;
        if (active_.size() + idle_.size() >= limits_.maxConnections) {
            // Room for a new one: drop the oldest idle connection, if any.
            if (!idle_.empty()) {
                auto oldest = idle_.front();
                idle_.erase(idle_.begin());
                oldest->detach();
                oldest->close();
            } else {
                respond(frontError(503, "the site is busy; try again shortly"));
                return;
            }
        }
        auto fd = connectUnix(socketPath_);
        if (!fd) {
            bool busy = fd.error().code == ErrorCode::WouldBlock;
            respond(frontError(busy ? 503 : 502, busy ? "the site is busy; try again shortly" : "the site isn't running"));
            return;
        }
        c = std::make_shared<UpstreamConnection>(*this, std::move(fd).value());
        if (!c->start()) {
            respond(frontError(502, "the site isn't running"));
            return;
        }
    }
    active_.push_back(c);
    c->send(std::move(wire), std::move(respond), retryable);
}

void Upstream::release(UpstreamConnection* c, bool reusable) {
    auto it = std::find_if(active_.begin(), active_.end(), [c](const auto& p) { return p.get() == c; });
    if (it == active_.end()) return;
    std::shared_ptr<UpstreamConnection> kept = std::move(*it);
    active_.erase(it);
    if (!reusable || idle_.size() >= limits_.maxIdle) {
        kept->detach();
        kept->close();
        return;
    }
    kept->park();
    idle_.push_back(std::move(kept));
}

void Upstream::forget(UpstreamConnection* c) {
    auto match = [c](const auto& p) { return p.get() == c; };
    active_.erase(std::remove_if(active_.begin(), active_.end(), match), active_.end());
    idle_.erase(std::remove_if(idle_.begin(), idle_.end(), match), idle_.end());
}

void Upstream::closeAll() {
    auto all = std::move(active_);
    for (auto& c : idle_) all.push_back(std::move(c));
    active_.clear();
    idle_.clear();
    for (auto& c : all) {
        c->detach();
        c->close();
    }
}

}  // namespace fourfold

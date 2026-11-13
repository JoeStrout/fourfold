#include "http2.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "connection.h"

// nghttp2 1.61 replaced the ssize_t functions with nghttp2_ssize ones, and
// deprecated the old; older releases (Ubuntu 24.04 has 1.59) have only those.
#if NGHTTP2_VERSION_NUM >= 0x013d00
#define FF_NGHTTP2_SSIZE 1
#endif

namespace fourfold {

namespace {

// Output waiting on the connection past which produce() stops.
constexpr size_t kOutputHighWater = 256 * 1024;

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Headers HTTP/2 forbids (RFC 9113 §8.2.2), and those the session writes.
bool droppedResponseHeader(const std::string& name) {
    return name == "connection" || name == "keep-alive" || name == "proxy-connection" ||
           name == "transfer-encoding" || name == "upgrade" || name == "content-length";
}

bool safeHeaderText(const std::string& s) {
    return s.find_first_of(std::string("\r\n\0", 3)) == std::string::npos;
}

nghttp2_nv nv(const std::string& name, const std::string& value) {
    nghttp2_nv n;
    n.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
    n.namelen = name.size();
    n.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
    n.valuelen = value.size();
    n.flags = NGHTTP2_NV_FLAG_NONE;
    return n;
}

}  // namespace

// The data source nghttp2 reads a response body through.
struct Http2DataSource {
#ifdef FF_NGHTTP2_SSIZE
    static nghttp2_ssize read(nghttp2_session*, int32_t id, uint8_t* buffer, size_t length, uint32_t* flags,
                              nghttp2_data_source*, void* user) {
        return static_cast<Http2Session*>(user)->readBody(id, buffer, length, flags);
    }
#else
    static ssize_t read(nghttp2_session*, int32_t id, uint8_t* buffer, size_t length, uint32_t* flags,
                        nghttp2_data_source*, void* user) {
        return static_cast<Http2Session*>(user)->readBody(id, buffer, length, flags);
    }
#endif
};

// A streamed response's writer: holds the connection weakly, and the stream
// by id.
class Http2StreamWriter : public StreamWriter {
public:
    Http2StreamWriter(std::weak_ptr<HttpConnection> connection, int32_t id)
        : connection_(std::move(connection)), id_(id) {}

    bool write(std::string bytes) override {
        auto c = connection_.lock();
        if (!c || !c->h2_) return false;
        bool ok = c->h2_->streamWrite(id_, std::move(bytes));
        if (!c->h2_->busy()) c->flush();
        return ok;
    }
    void close() override {
        auto c = connection_.lock();
        if (!c || !c->h2_) return;
        c->h2_->streamEnd(id_);
        if (!c->h2_->busy()) c->flush();
    }
    bool isOpen() const override {
        auto c = connection_.lock();
        return c && c->h2_ && c->h2_->streamOpen(id_);
    }

private:
    std::weak_ptr<HttpConnection> connection_;
    int32_t id_;
};

Http2Session::Http2Session(HttpConnection& connection)
    : connection_(connection), idleSince_(std::chrono::steady_clock::now()) {}

Http2Session::~Http2Session() {
    closed();
    if (session_) nghttp2_session_del(session_);
}

bool Http2Session::start() {
    nghttp2_session_callbacks* callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) return false;
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, onBeginHeaders);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, onHeader);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, onFrameReceived);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, onDataChunk);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, onFrameSent);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, onStreamClose);
    nghttp2_option* option = nullptr;
    nghttp2_option_new(&option);
    const HttpServerLimits& limits = connection_.limits_;
    // HPACK can make a small block decode to a large list; this bounds it
    // (each field counts its name, value and 32 bytes).
    nghttp2_option_set_max_send_header_block_length(option, 64 * 1024);
    int rv = nghttp2_session_server_new2(&session_, callbacks, this, option);
    nghttp2_option_del(option);
    nghttp2_session_callbacks_del(callbacks);
    if (rv != 0) return false;

    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, limits.maxConcurrentStreams},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
         static_cast<uint32_t>(limits.maxHeaderBytes + 32 * limits.maxHeaderCount)},
        {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
    };
    return nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, sizeof settings / sizeof settings[0]) == 0;
}

bool Http2Session::receive(const std::string& data) {
    busy_ = true;
#ifdef FF_NGHTTP2_SSIZE
    nghttp2_ssize n = nghttp2_session_mem_recv2(session_, reinterpret_cast<const uint8_t*>(data.data()), data.size());
#else
    ssize_t n = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(data.data()), data.size());
#endif
    busy_ = false;
    for (int32_t id : early_) {
        if (Stream* s = find(id)) answer(*s, s->errorStatus);
    }
    early_.clear();
    if (n < 0) {
        // nghttp2 has queued a GOAWAY for most errors; send what it has.
        if (n != NGHTTP2_ERR_BAD_CLIENT_MAGIC) {
            nghttp2_session_terminate_session(session_, NGHTTP2_PROTOCOL_ERROR);
        }
        produce();
        return false;
    }
    return true;
}

bool Http2Session::produce() {
    if (!session_ || busy_) return false;
    bool any = false;
    while (connection_.outbuf_.size() - connection_.outSent_ < kOutputHighWater) {
        const uint8_t* data = nullptr;
        busy_ = true;
#ifdef FF_NGHTTP2_SSIZE
        nghttp2_ssize n = nghttp2_session_mem_send2(session_, &data);
#else
        ssize_t n = nghttp2_session_mem_send(session_, &data);
#endif
        busy_ = false;
        if (n <= 0) break;
        connection_.queue(reinterpret_cast<const char*>(data), static_cast<size_t>(n));
        any = true;
    }
    return any;
}

bool Http2Session::finished() const {
    return !session_ || (!nghttp2_session_want_read(session_) && !nghttp2_session_want_write(session_));
}

void Http2Session::goAway() {
    if (goingAway_ || !session_) return;
    goingAway_ = true;
    nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, nghttp2_session_get_last_proc_stream_id(session_),
                          NGHTTP2_NO_ERROR, nullptr, 0);
    for (auto& [id, s] : streams_) {
        if (s->streaming && !s->outEnd) streamEnd(id);
    }
}

void Http2Session::checkTimers() {
    auto now = std::chrono::steady_clock::now();
    const HttpServerLimits& limits = connection_.limits_;
    std::vector<int32_t> late;
    for (auto& [id, s] : streams_) {
        if (s->dispatched) continue;
        if (!s->headersDone) {
            if (now - s->started >= limits.headerTimeout) late.push_back(id);
            continue;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s->bodyStart);
        double expected = static_cast<double>(limits.minBodyBytesPerSecond) *
                          static_cast<double>((elapsed - limits.bodyGrace).count()) / 1000.0;
        if (static_cast<double>(s->bodyBytes) < expected) late.push_back(id);
    }
    for (int32_t id : late) {
        if (Stream* s = find(id)) answer(*s, 408);
    }
}

void Http2Session::closed() {
    for (auto& [id, s] : streams_) release(*s);
    streams_.clear();
}

Http2Session::Stream* Http2Session::find(int32_t id) const {
    auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : it->second.get();
}

void Http2Session::release(Stream& s) {
    if (!s.counted) return;
    s.counted = false;
    connection_.server_.endStream(s.clientAddress);
}

void Http2Session::complete(Stream& s) {
    s.requestDone = true;
    if (s.dispatched) return;
    if (s.errorStatus) {
        answer(s, s.errorStatus);
        return;
    }
    s.dispatched = true;
    HttpRequest request = std::move(s.request);
    s.request = HttpRequest{};
    request.versionMajor = 2;
    request.versionMinor = 0;
    request.peerAddress = connection_.peerAddress_;
    s.head = request.method == "HEAD";
    HttpServer& server = connection_.server_;
    s.clientAddress = server.clientAddress_ ? server.clientAddress_(request) : connection_.peerAddress_;
    std::weak_ptr<HttpConnection> weak = connection_.shared_from_this();
    int32_t id = s.id;
    server.handler_(std::move(request), [weak, id](HttpResponse response) {
        auto c = weak.lock();
        if (!c || !c->h2_) return;
        c->h2_->respond(id, std::move(response));
        if (!c->h2_->busy()) c->flush();
    });
}

void Http2Session::answer(Stream& s, int status) {
    s.dispatched = true;
    respond(s.id, HttpResponse::text(status, std::string(statusReason(status)) + "\n"));
}

void Http2Session::respond(int32_t id, HttpResponse response) {
    Stream* s = find(id);
    if (!s || s->responded || !session_) return;
    s->responded = true;

    int status = response.status;
    bool mayHaveBody = !(status < 200 || status == 204 || status == 304);
    bool streamed = response.stream && mayHaveBody && !s->head;
    if (streamed && !connection_.server_.startStream(s->clientAddress)) {
        response = HttpResponse::text(503, "Service Unavailable: too many open event streams\n");
        response.headers.set("Retry-After", "10");
        status = 503;
        streamed = false;
    } else if (streamed) {
        s->counted = true;
        s->streaming = true;
    }

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back(":status", std::to_string(status));
    for (const Headers::Field& f : response.headers) {
        std::string name = lowered(f.name);
        if (droppedResponseHeader(name) || name.empty() || name[0] == ':' || !safeHeaderText(name) ||
            !safeHeaderText(f.value)) {
            continue;
        }
        fields.emplace_back(std::move(name), f.value);
    }
    if (mayHaveBody && !streamed) fields.emplace_back("content-length", std::to_string(response.body.size()));
    std::vector<nghttp2_nv> nva;
    for (const auto& [name, value] : fields) nva.push_back(nv(name, value));

    bool body = streamed || (mayHaveBody && !s->head && !response.body.empty());
    if (body && !streamed) {
        s->out = std::move(response.body);
        s->outEnd = true;
    }
    int rv;
#ifdef FF_NGHTTP2_SSIZE
    nghttp2_data_provider2 provider;
    provider.source.ptr = nullptr;
    provider.read_callback = Http2DataSource::read;
    rv = nghttp2_submit_response2(session_, id, nva.data(), nva.size(), body ? &provider : nullptr);
#else
    nghttp2_data_provider provider;
    provider.source.ptr = nullptr;
    provider.read_callback = Http2DataSource::read;
    rv = nghttp2_submit_response(session_, id, nva.data(), nva.size(), body ? &provider : nullptr);
#endif
    if (rv != 0) {
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, id, NGHTTP2_INTERNAL_ERROR);
        return;
    }
    if (streamed) {
        auto writer = std::make_shared<Http2StreamWriter>(connection_.weak_from_this(), id);
        response.stream(writer);
        if (goingAway_) streamEnd(id);
    }
}

bool Http2Session::streamWrite(int32_t id, std::string bytes) {
    Stream* s = find(id);
    if (!s || !s->streaming || s->outEnd) return false;
    if (s->out.size() - s->outOffset + bytes.size() > connection_.limits_.maxStreamBufferBytes) {
        // The client isn't reading this stream.
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
        s->outEnd = true;
        return false;
    }
    s->out += bytes;
    resume(*s);
    return true;
}

void Http2Session::streamEnd(int32_t id) {
    Stream* s = find(id);
    if (!s || !s->streaming || s->outEnd) return;
    s->outEnd = true;
    resume(*s);
}

bool Http2Session::streamOpen(int32_t id) const {
    Stream* s = find(id);
    return s && s->streaming && !s->outEnd;
}

void Http2Session::resume(Stream& s) {
    if (!s.deferred) return;
    s.deferred = false;
    nghttp2_session_resume_data(session_, s.id);
}

long Http2Session::readBody(int32_t id, uint8_t* buffer, size_t length, uint32_t* flags) {
    Stream* s = find(id);
    if (!s) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    size_t available = s->out.size() - s->outOffset;
    size_t n = std::min(available, length);
    if (n > 0) std::memcpy(buffer, s->out.data() + s->outOffset, n);
    s->outOffset += n;
    if (s->outOffset == s->out.size()) {
        s->out.clear();
        s->outOffset = 0;
    } else if (s->outOffset > 64 * 1024) {
        s->out.erase(0, s->outOffset);
        s->outOffset = 0;
    }
    if (s->out.empty() && s->outEnd) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<long>(n);
    }
    if (n == 0) {
        s->deferred = true;
        return NGHTTP2_ERR_DEFERRED;
    }
    return static_cast<long>(n);
}

int Http2Session::onBeginHeaders(nghttp2_session* session, const nghttp2_frame* frame, void* user) {
    auto* self = static_cast<Http2Session*>(user);
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    if (self->goingAway_) {
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id, NGHTTP2_REFUSED_STREAM);
        return 0;
    }
    auto s = std::make_unique<Stream>();
    s->id = frame->hd.stream_id;
    s->started = std::chrono::steady_clock::now();
    s->bodyLimit = self->connection_.limits_.maxBodyBytes;
    self->streams_[s->id] = std::move(s);
    return 0;
}

int Http2Session::onHeader(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t nameLength,
                           const uint8_t* value, size_t valueLength, uint8_t, void* user) {
    auto* self = static_cast<Http2Session*>(user);
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    Stream* s = self->find(frame->hd.stream_id);
    if (!s || s->errorStatus) return 0;
    const HttpServerLimits& limits = self->connection_.limits_;
    s->headerBytes += nameLength + valueLength;
    if (s->headerBytes > limits.maxHeaderBytes) {
        s->errorStatus = 431;
        return 0;
    }
    std::string n(reinterpret_cast<const char*>(name), nameLength);
    std::string v(reinterpret_cast<const char*>(value), valueLength);
    HttpRequest& r = s->request;
    if (n == ":method") {
        r.method = v;
    } else if (n == ":path") {
        r.target = v;
    } else if (n == ":authority") {
        if (!r.headers.has("Host")) r.headers.add("Host", v);
    } else if (n[0] == ':') {
        // :scheme, :protocol
    } else if (n == "cookie" && r.headers.has("Cookie")) {
        // HTTP/2 may split cookies into fields; HTTP/1.1 has them in one.
        std::string joined = *r.headers.get("Cookie") + "; " + v;
        r.headers.set("Cookie", std::move(joined));
    } else {
        if (r.headers.size() >= limits.maxHeaderCount) {
            s->errorStatus = 431;
            return 0;
        }
        if (n == "host") {
            r.headers.set("Host", v);   // over :authority
        } else {
            r.headers.add(n == "cookie" ? "Cookie" : n, v);
        }
    }
    return 0;
}

int Http2Session::onFrameReceived(nghttp2_session*, const nghttp2_frame* frame, void* user) {
    auto* self = static_cast<Http2Session*>(user);
    Stream* s = self->find(frame->hd.stream_id);
    if (!s) return 0;
    bool endStream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
    if (frame->hd.type == NGHTTP2_HEADERS && !s->headersDone) {
        s->headersDone = true;
        s->bodyStart = std::chrono::steady_clock::now();
        const HttpServerLimits& limits = self->connection_.limits_;
        if (const std::string* type = s->request.headers.get("content-type")) {
            std::string media = type->substr(0, type->find(';'));
            while (!media.empty() && media.back() == ' ') media.pop_back();
            if (equalsIgnoreCase(media, "multipart/form-data")) s->bodyLimit = limits.maxUploadBodyBytes;
        }
        if (const std::string* length = s->request.headers.get("content-length")) {
            bool digits = !length->empty() && length->size() < 19 &&
                          length->find_first_not_of("0123456789") == std::string::npos;
            if (!digits || std::stoull(*length) > s->bodyLimit) {
                if (!s->errorStatus) s->errorStatus = digits ? 413 : 400;
            }
        }
        // Answer a refused request now, without waiting for its body
        // (once nghttp2 is done with the frame).
        if (s->errorStatus) {
            s->dispatched = true;
            if (endStream) s->requestDone = true;
            self->early_.push_back(s->id);
            return 0;
        }
    }
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) && endStream) self->complete(*s);
    return 0;
}

int Http2Session::onDataChunk(nghttp2_session*, uint8_t, int32_t id, const uint8_t* data, size_t length,
                              void* user) {
    auto* self = static_cast<Http2Session*>(user);
    Stream* s = self->find(id);
    if (!s || s->dispatched) return 0;   // a refused request's body is dropped
    if (s->request.body.size() + length > s->bodyLimit) {
        s->errorStatus = 413;
        s->dispatched = true;
        self->early_.push_back(s->id);
        return 0;
    }
    s->request.body.append(reinterpret_cast<const char*>(data), length);
    s->bodyBytes += length;
    return 0;
}

int Http2Session::onFrameSent(nghttp2_session* session, const nghttp2_frame* frame, void* user) {
    auto* self = static_cast<Http2Session*>(user);
    if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) return 0;
    if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) return 0;
    Stream* s = self->find(frame->hd.stream_id);
    // Answered before the client finished sending (a refused upload): stop
    // it sending the rest (RFC 9113 §8.1).
    if (s && !s->requestDone) nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, s->id, NGHTTP2_NO_ERROR);
    return 0;
}

int Http2Session::onStreamClose(nghttp2_session*, int32_t id, uint32_t, void* user) {
    auto* self = static_cast<Http2Session*>(user);
    auto it = self->streams_.find(id);
    if (it == self->streams_.end()) return 0;
    self->release(*it->second);
    self->streams_.erase(it);
    if (self->streams_.empty()) self->idleSince_ = std::chrono::steady_clock::now();
    return 0;
}

}  // namespace fourfold

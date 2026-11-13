// HTTP/2 for one HttpConnection (after ALPN chose "h2"), over nghttp2.
//
// The session takes the connection's decrypted input, hands each complete
// request to the server's handler (streams are handled concurrently), and
// makes output for the connection to send. The same limits apply as for
// HTTP/1.1: header size and count, body sizes, the header timeout and the
// minimum body rate (per stream), and the stream caps for server-sent
// events. nghttp2 checks the protocol itself (pseudo-headers,
// connection-specific headers, Content-Length, flow control), and limits
// how fast a client may reset streams.
#pragma once

#include <nghttp2/nghttp2.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "http/message.h"

namespace fourfold {

class HttpConnection;
class Http2StreamWriter;

class Http2Session {
public:
    explicit Http2Session(HttpConnection& connection);
    ~Http2Session();

    Http2Session(const Http2Session&) = delete;
    Http2Session& operator=(const Http2Session&) = delete;

    // Sends the server's SETTINGS. False if nghttp2 couldn't start.
    bool start();
    // Takes decrypted input. False on a protocol error (the GOAWAY is
    // queued; send it, then close).
    bool receive(const std::string& data);
    // Moves output into the connection's buffer, while it isn't too full.
    // True if it moved any.
    bool produce();
    // Nothing more to read or write: close the connection.
    bool finished() const;
    // No streams open, and since when.
    bool idle() const { return streams_.empty(); }
    std::chrono::steady_clock::time_point idleSince() const { return idleSince_; }
    // Graceful shutdown: GOAWAY, so the client opens no more streams; those
    // open finish (streamed responses are ended).
    void goAway();
    // Header timeouts and minimum body rates, for streams still arriving.
    void checkTimers();
    // The connection has closed: let go of the streams.
    void closed();

    // A stream's response, from the handler. Ignored if the stream is gone.
    void respond(int32_t id, HttpResponse response);
    // Server-sent events on a stream.
    bool streamWrite(int32_t id, std::string bytes);
    void streamEnd(int32_t id);
    bool streamOpen(int32_t id) const;
    // Whether nghttp2 is running a callback (output must wait).
    bool busy() const { return busy_; }

private:
    struct Stream {
        int32_t id = 0;
        HttpRequest request;
        std::chrono::steady_clock::time_point started, bodyStart;
        size_t headerBytes = 0;
        size_t bodyLimit = 0;
        size_t bodyBytes = 0;
        int errorStatus = 0;         // answer this instead of handling it
        bool headersDone = false;
        bool requestDone = false;    // the client's side has ended
        bool dispatched = false;     // handed to the handler (or answered)
        bool responded = false;
        bool head = false;
        // The response body, as the data source reads it.
        std::string out;
        size_t outOffset = 0;
        bool outEnd = false;
        bool deferred = false;
        // A streamed response (server-sent events).
        bool streaming = false;
        bool counted = false;        // against the server's stream caps
        std::string clientAddress;
    };

    Stream* find(int32_t id) const;
    void complete(Stream& s);
    void answer(Stream& s, int status);
    void release(Stream& s);
    void resume(Stream& s);
    // Bytes of response body for nghttp2.
    long readBody(int32_t id, uint8_t* buffer, size_t length, uint32_t* flags);

    static int onBeginHeaders(nghttp2_session*, const nghttp2_frame* frame, void* user);
    static int onHeader(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t nameLength,
                        const uint8_t* value, size_t valueLength, uint8_t flags, void* user);
    static int onFrameReceived(nghttp2_session*, const nghttp2_frame* frame, void* user);
    static int onDataChunk(nghttp2_session*, uint8_t flags, int32_t id, const uint8_t* data, size_t length,
                           void* user);
    static int onFrameSent(nghttp2_session*, const nghttp2_frame* frame, void* user);
    static int onStreamClose(nghttp2_session*, int32_t id, uint32_t errorCode, void* user);
    friend struct Http2DataSource;

    HttpConnection& connection_;
    nghttp2_session* session_ = nullptr;
    std::map<int32_t, std::unique_ptr<Stream>> streams_;
    std::chrono::steady_clock::time_point idleSince_;
    std::vector<int32_t> early_;   // streams to refuse once the input is processed
    bool busy_ = false;
    bool goingAway_ = false;
};

}  // namespace fourfold

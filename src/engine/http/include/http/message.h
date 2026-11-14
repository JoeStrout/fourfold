// HttpRequest and HttpResponse: the seam between transports (the standalone
// server, CGI, FastCGI, the test runner) and the engine (Arch §6). Plain
// structs, so the engine can't tell which transport a request came from.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/fd.h"

namespace fourfold {

// An ordered list of header fields. Names compare case-insensitively.
class Headers {
public:
    struct Field {
        std::string name;
        std::string value;
    };

    // Appends a field, keeping any others of the same name.
    void add(std::string name, std::string value);
    // Replaces all fields of this name with one.
    void set(const std::string& name, std::string value);
    void remove(const std::string& name);

    // The first field of this name, or nullptr.
    const std::string* get(const std::string& name) const;
    bool has(const std::string& name) const { return get(name) != nullptr; }

    size_t size() const { return fields_.size(); }
    std::vector<Field>::const_iterator begin() const { return fields_.begin(); }
    std::vector<Field>::const_iterator end() const { return fields_.end(); }

private:
    std::vector<Field> fields_;
};

bool equalsIgnoreCase(const std::string& a, const std::string& b);

struct HttpRequest {
    std::string method;          // "GET", "POST", …
    std::string target;          // as sent: "/path?query" (not yet canonicalized)
    int versionMajor = 1;
    int versionMinor = 1;
    Headers headers;
    std::string body;            // fully read, within the transport's limits
    std::string peerAddress;     // numeric IP of the connection's other end
};

// The writing end of a streamed response body (server-sent events). Its
// methods must be called on the transport's thread (for HttpServer, its
// loop). Once the client has gone, isOpen() is false and writes are dropped.
class StreamWriter {
public:
    virtual ~StreamWriter() = default;
    // False if the stream is closed, or the client isn't keeping up (it's
    // then closed).
    virtual bool write(std::string bytes) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
};

// A response body of known length that the transport reads a piece at a
// time, as it can send it (M11.2): a large static file, read from disk; or a
// large response `fourfold front` passes on from an engine as it arrives.
// Its methods are called on the transport's thread.
class BodySource {
public:
    enum class Status {
        Data,     // bytes were appended
        End,      // the whole body has been read
        Wait,     // nothing yet: the wake function is called when there may be
        Failed,   // the body can't be finished; the transport closes the connection
    };
    virtual ~BodySource() = default;
    // The body's length in bytes: its Content-Length.
    virtual uint64_t size() const = 0;
    // Appends at most `max` bytes to `out`.
    virtual Status read(std::string& out, size_t max) = 0;
    // Sets what to call when a read that said Wait may make progress. Sources
    // that never wait ignore it.
    virtual void setWake(std::function<void()> wake) { (void)wake; }
};

// Bytes [offset, offset + length) of an open file. Fails if the file ends
// sooner (it was cut short while being sent).
std::shared_ptr<BodySource> fileBodySource(Fd fd, uint64_t offset, uint64_t length);

// Reads all of `source` into `out` (for transports and tests that can't
// stream). False if it fails, waits, or would pass `maxBytes`.
bool readWholeBody(BodySource& source, std::string& out, size_t maxBytes);

struct HttpResponse {
    int status = 200;
    Headers headers;
    std::string body;
    // If set, the body is read from here instead (`body` is empty), and its
    // size is the Content-Length.
    std::shared_ptr<BodySource> source;
    // If set, the body is streamed instead: after the headers are sent, the
    // transport calls this, on its own thread, with the writer to send the
    // body through. The connection ends when the stream does.
    std::function<void(std::shared_ptr<StreamWriter>)> stream;

    // A plain-text response, e.g. for errors.
    static HttpResponse text(int status, std::string body);
};

// "OK", "Not Found", …; "Unknown" for codes we don't list.
const char* statusReason(int status);

// The length of the response's body: its source's size, or its body's.
uint64_t bodySize(const HttpResponse& response);

// An HTTP date (RFC 9110 §5.6.7, IMF-fixdate): "Sun, 06 Nov 1994 08:49:37 GMT".
std::string httpDate(int64_t unixSeconds);
// Reads one: IMF-fixdate, or the obsolete RFC 850 and asctime forms, which
// recipients must also accept. nullopt if it's none of them.
std::optional<int64_t> parseHttpDate(const std::string& text);

}  // namespace fourfold

// FastCGI (the FastCGI 1.0 specification), the responder role only: what a
// web server's FastCGI module (mod_fcgid, mod_proxy_fcgi) speaks to a
// long-running process (overview §8, Hosting modes; M10).
//
// A connection carries one request at a time (Fourfold says so when asked:
// FCGI_MPXS_CONNS is 0), which is how mod_fcgid and mod_proxy_fcgi use it;
// a second request while one is open is refused (FCGI_CANT_MPX_CONN). The
// request's params are the CGI variables, and its stdin the body; the
// response goes out as CGI output on stdout.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/result.h"
#include "cgi/cgi_request.h"

namespace fourfold::fastcgi {

enum RecordType : uint8_t {
    kBeginRequest = 1,
    kAbortRequest = 2,
    kEndRequest = 3,
    kParams = 4,
    kStdin = 5,
    kStdout = 6,
    kStderr = 7,
    kData = 8,
    kGetValues = 9,
    kGetValuesResult = 10,
    kUnknownType = 11,
};

constexpr uint16_t kResponder = 1;
constexpr uint8_t kKeepConn = 1;
constexpr uint8_t kRequestComplete = 0;
constexpr uint8_t kCantMpxConn = 1;
constexpr uint8_t kUnknownRole = 3;

struct Record {
    uint8_t type = 0;
    uint16_t requestId = 0;
    std::string content;
};

// One record, ready to send: the header, `content` (at most 65535 bytes),
// and padding to a multiple of 8.
std::string encodeRecord(uint8_t type, uint16_t requestId, std::string_view content);
// A stream (stdout) as records: as many as it takes, then the empty one
// that ends it.
std::string encodeStream(uint8_t type, uint16_t requestId, std::string_view data);
// FCGI_END_REQUEST's body.
std::string encodeEndRequest(uint16_t requestId, uint32_t appStatus, uint8_t protocolStatus);

// Name-value pairs (params, and FCGI_GET_VALUES), into `out`. False if
// they're malformed.
bool decodePairs(std::string_view data, CgiVariables& out);
std::string encodePairs(const CgiVariables& pairs);

// Reads records from a byte stream as they arrive.
class RecordReader {
public:
    // Adds bytes read from the connection.
    void feed(std::string_view bytes) { buffer_.append(bytes.data(), bytes.size()); }
    // The next whole record, if one has arrived; an error if the stream
    // isn't FastCGI (a version other than 1).
    Result<std::optional<Record>> next();

private:
    std::string buffer_;
};

// Serves the requests on one connection (`fd`, blocking), one at a time,
// until the web server closes it, or it asks for it to be closed after a
// request (no FCGI_KEEP_CONN). `handle` gets each request's variables and
// body. Bodies over `limits` are refused with a 413 without being kept.
// Returns when the connection is done; an error if it broke the protocol
// or a read or write failed.
using Handler = std::function<HttpResponse(const CgiVariables& vars, std::string body)>;
Status serveConnection(int fd, const Handler& handle, const CgiLimits& limits = {});

}  // namespace fourfold::fastcgi

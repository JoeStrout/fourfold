#include "cgi/fastcgi.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace fourfold::fastcgi {

namespace {

constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxContent = 65535;
// Stdout records carry this much at most: a multiple of 8, so they need no
// padding.
constexpr size_t kChunk = 65528;

void putLength(std::string& out, size_t n) {
    if (n < 128) {
        out += static_cast<char>(n);
    } else {
        out += static_cast<char>(((n >> 24) & 0x7F) | 0x80);
        out += static_cast<char>((n >> 16) & 0xFF);
        out += static_cast<char>((n >> 8) & 0xFF);
        out += static_cast<char>(n & 0xFF);
    }
}

bool getLength(std::string_view data, size_t& at, size_t& n) {
    if (at >= data.size()) return false;
    unsigned char b = static_cast<unsigned char>(data[at]);
    if (b < 128) {
        n = b;
        at += 1;
        return true;
    }
    if (at + 4 > data.size()) return false;
    n = (static_cast<size_t>(b & 0x7F) << 24) | (static_cast<size_t>(static_cast<unsigned char>(data[at + 1])) << 16) |
        (static_cast<size_t>(static_cast<unsigned char>(data[at + 2])) << 8) |
        static_cast<size_t>(static_cast<unsigned char>(data[at + 3]));
    at += 4;
    return true;
}

Status writeAll(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return makeError(ErrorCode::IO, std::string("FastCGI write: ") + std::strerror(errno));
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return {};
}

}  // namespace

std::string encodeRecord(uint8_t type, uint16_t requestId, std::string_view content) {
    size_t length = std::min(content.size(), kMaxContent);
    size_t padding = (8 - length % 8) % 8;
    std::string out;
    out.reserve(kHeaderSize + length + padding);
    out += static_cast<char>(1);   // version
    out += static_cast<char>(type);
    out += static_cast<char>(requestId >> 8);
    out += static_cast<char>(requestId & 0xFF);
    out += static_cast<char>(length >> 8);
    out += static_cast<char>(length & 0xFF);
    out += static_cast<char>(padding);
    out += '\0';
    out.append(content.data(), length);
    out.append(padding, '\0');
    return out;
}

std::string encodeStream(uint8_t type, uint16_t requestId, std::string_view data) {
    std::string out;
    while (!data.empty()) {
        size_t n = std::min(data.size(), kChunk);
        out += encodeRecord(type, requestId, data.substr(0, n));
        data.remove_prefix(n);
    }
    out += encodeRecord(type, requestId, {});
    return out;
}

std::string encodeEndRequest(uint16_t requestId, uint32_t appStatus, uint8_t protocolStatus) {
    std::string body;
    body += static_cast<char>((appStatus >> 24) & 0xFF);
    body += static_cast<char>((appStatus >> 16) & 0xFF);
    body += static_cast<char>((appStatus >> 8) & 0xFF);
    body += static_cast<char>(appStatus & 0xFF);
    body += static_cast<char>(protocolStatus);
    body.append(3, '\0');
    return encodeRecord(kEndRequest, requestId, body);
}

bool decodePairs(std::string_view data, CgiVariables& out) {
    size_t at = 0;
    while (at < data.size()) {
        size_t nameLength = 0, valueLength = 0;
        if (!getLength(data, at, nameLength) || !getLength(data, at, valueLength)) return false;
        if (nameLength > data.size() - at || valueLength > data.size() - at - nameLength) return false;
        std::string name(data.substr(at, nameLength));
        at += nameLength;
        out[std::move(name)] = std::string(data.substr(at, valueLength));
        at += valueLength;
    }
    return true;
}

std::string encodePairs(const CgiVariables& pairs) {
    std::string out;
    for (const auto& [name, value] : pairs) {
        putLength(out, name.size());
        putLength(out, value.size());
        out += name;
        out += value;
    }
    return out;
}

Result<std::optional<Record>> RecordReader::next() {
    if (buffer_.size() < kHeaderSize) return std::optional<Record>();
    const unsigned char* h = reinterpret_cast<const unsigned char*>(buffer_.data());
    if (h[0] != 1) return makeError(ErrorCode::Invalid, "not FastCGI (version " + std::to_string(h[0]) + ")");
    size_t length = (static_cast<size_t>(h[4]) << 8) | h[5];
    size_t padding = h[6];
    if (buffer_.size() < kHeaderSize + length + padding) return std::optional<Record>();
    Record r;
    r.type = h[1];
    r.requestId = static_cast<uint16_t>((h[2] << 8) | h[3]);
    r.content = buffer_.substr(kHeaderSize, length);
    buffer_.erase(0, kHeaderSize + length + padding);
    return std::optional<Record>(std::move(r));
}

Status serveConnection(int fd, const Handler& handle, const CgiLimits& limits) {
    RecordReader reader;
    struct Request {
        uint16_t id = 0;
        bool keepConn = false;
        std::string params;
        CgiVariables vars;
        bool paramsDone = false;
        std::string body;
        size_t limit = 0;
        bool tooLarge = false;
        bool badLength = false;
    };
    std::optional<Request> current;
    char buffer[16 * 1024];

    while (true) {
        auto record = reader.next();
        if (!record) return record.error();
        if (!record.value()) {
            ssize_t n = ::read(fd, buffer, sizeof buffer);
            if (n < 0) {
                if (errno == EINTR) continue;
                return makeError(ErrorCode::IO, std::string("FastCGI read: ") + std::strerror(errno));
            }
            if (n == 0) return {};   // the web server closed the connection
            reader.feed(std::string_view(buffer, static_cast<size_t>(n)));
            continue;
        }
        Record& r = *record.value();

        // Management records (request id 0).
        if (r.requestId == 0) {
            if (r.type == kGetValues) {
                CgiVariables asked, answer;
                if (!decodePairs(r.content, asked)) return makeError(ErrorCode::Invalid, "bad FCGI_GET_VALUES");
                for (const auto& [name, value] : asked) {
                    if (name == "FCGI_MAX_CONNS" || name == "FCGI_MAX_REQS") answer[name] = "1";
                    if (name == "FCGI_MPXS_CONNS") answer[name] = "0";
                }
                if (Status s = writeAll(fd, encodeRecord(kGetValuesResult, 0, encodePairs(answer))); !s) return s;
            } else {
                std::string body(8, '\0');
                body[0] = static_cast<char>(r.type);
                if (Status s = writeAll(fd, encodeRecord(kUnknownType, 0, body)); !s) return s;
            }
            continue;
        }

        if (r.type == kBeginRequest) {
            if (r.content.size() < 8) return makeError(ErrorCode::Invalid, "bad FCGI_BEGIN_REQUEST");
            uint16_t role = static_cast<uint16_t>((static_cast<unsigned char>(r.content[0]) << 8) |
                                                  static_cast<unsigned char>(r.content[1]));
            bool keep = (static_cast<unsigned char>(r.content[2]) & kKeepConn) != 0;
            if (current) {
                if (Status s = writeAll(fd, encodeEndRequest(r.requestId, 0, kCantMpxConn)); !s) return s;
                continue;
            }
            if (role != kResponder) {
                if (Status s = writeAll(fd, encodeEndRequest(r.requestId, 0, kUnknownRole)); !s) return s;
                if (!keep) return {};
                continue;
            }
            current = Request{};
            current->id = r.requestId;
            current->keepConn = keep;
            continue;
        }
        if (!current || r.requestId != current->id) continue;   // a request we don't know: ignored

        if (r.type == kAbortRequest) {
            bool keep = current->keepConn;
            uint16_t id = current->id;
            current.reset();
            if (Status s = writeAll(fd, encodeEndRequest(id, 0, kRequestComplete)); !s) return s;
            if (!keep) return {};
            continue;
        }
        if (r.type == kParams) {
            if (current->paramsDone) continue;
            if (!r.content.empty()) {
                current->params += r.content;
                if (current->params.size() > 1024 * 1024) return makeError(ErrorCode::TooLarge, "FastCGI params too large");
                continue;
            }
            if (!decodePairs(current->params, current->vars)) return makeError(ErrorCode::Invalid, "bad FastCGI params");
            current->params.clear();
            current->paramsDone = true;
            bool tooLarge = false;
            auto length = cgiBodyLength(current->vars, limits, &tooLarge);
            current->tooLarge = tooLarge;
            current->badLength = !length && !tooLarge;
            current->limit = length.value_or(0);
            continue;
        }
        if (r.type == kStdin) {
            if (!r.content.empty()) {
                if (!current->tooLarge && !current->badLength) {
                    if (current->body.size() + r.content.size() > current->limit) {
                        current->tooLarge = true;   // more than CONTENT_LENGTH said
                        current->body.clear();
                    } else {
                        current->body += r.content;
                    }
                }
                continue;
            }
            // The end of stdin: the request is complete.
            HttpResponse response = current->tooLarge  ? HttpResponse::text(413, "Payload Too Large\n")
                                    : current->badLength ? HttpResponse::text(400, "Bad Request\n")
                                                         : handle(current->vars, std::move(current->body));
            std::string out = encodeStream(kStdout, current->id, cgiResponse(response));
            out += encodeEndRequest(current->id, 0, kRequestComplete);
            bool keep = current->keepConn;
            current.reset();
            if (Status s = writeAll(fd, out); !s) return s;
            if (!keep) return {};
            continue;
        }
        // kData (the filter role's) and anything else: ignored.
    }
}

}  // namespace fourfold::fastcgi

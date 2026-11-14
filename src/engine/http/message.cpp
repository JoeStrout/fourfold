#include "http/message.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace fourfold {

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

void Headers::add(std::string name, std::string value) {
    fields_.push_back({std::move(name), std::move(value)});
}

void Headers::set(const std::string& name, std::string value) {
    remove(name);
    add(name, std::move(value));
}

void Headers::remove(const std::string& name) {
    fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                 [&](const Field& f) { return equalsIgnoreCase(f.name, name); }),
                  fields_.end());
}

const std::string* Headers::get(const std::string& name) const {
    for (const Field& f : fields_) {
        if (equalsIgnoreCase(f.name, name)) return &f.value;
    }
    return nullptr;
}

HttpResponse HttpResponse::text(int status, std::string body) {
    HttpResponse r;
    r.status = status;
    r.headers.set("Content-Type", "text/plain; charset=utf-8");
    r.body = std::move(body);
    return r;
}

const char* statusReason(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
    }
    return "Unknown";
}

uint64_t bodySize(const HttpResponse& response) {
    return response.source ? response.source->size() : response.body.size();
}

namespace {

class FileBodySource : public BodySource {
public:
    FileBodySource(Fd fd, uint64_t offset, uint64_t length) : fd_(std::move(fd)), offset_(offset), length_(length) {}

    uint64_t size() const override { return length_; }

    Status read(std::string& out, size_t max) override {
        if (sent_ == length_) return Status::End;
        size_t want = static_cast<size_t>(std::min<uint64_t>(max, length_ - sent_));
        size_t at = out.size();
        out.resize(at + want);
        for (;;) {
            ssize_t n = ::pread(fd_.get(), &out[at], want, static_cast<off_t>(offset_ + sent_));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {   // an error, or the file is shorter than it was
                out.resize(at);
                return Status::Failed;
            }
            out.resize(at + static_cast<size_t>(n));
            sent_ += static_cast<uint64_t>(n);
            return Status::Data;
        }
    }

private:
    Fd fd_;
    uint64_t offset_;
    uint64_t length_;
    uint64_t sent_ = 0;
};

const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

int monthIndex(const std::string& m) {
    for (int i = 0; i < 12; i++) {
        if (m == kMonths[i]) return i;
    }
    return -1;
}

}  // namespace

std::shared_ptr<BodySource> fileBodySource(Fd fd, uint64_t offset, uint64_t length) {
    return std::make_shared<FileBodySource>(std::move(fd), offset, length);
}

bool readWholeBody(BodySource& source, std::string& out, size_t maxBytes) {
    if (source.size() > maxBytes) return false;
    for (;;) {
        BodySource::Status s = source.read(out, 256 * 1024);
        if (s == BodySource::Status::End) return true;
        if (s != BodySource::Status::Data || out.size() > maxBytes) return false;
    }
}

std::string httpDate(int64_t unixSeconds) {
    time_t t = static_cast<time_t>(unixSeconds);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[tm.tm_wday], tm.tm_mday,
                  kMonths[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

std::optional<int64_t> parseHttpDate(const std::string& text) {
    struct tm tm {};
    char day[16] = {}, month[4] = {}, zone[4] = {};
    int consumed = 0;
    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    if (std::sscanf(text.c_str(), "%3s, %2d %3s %4d %2d:%2d:%2d %3s%n", day, &tm.tm_mday, month, &tm.tm_year,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec, zone, &consumed) == 8 &&
        static_cast<size_t>(consumed) == text.size() && std::strcmp(zone, "GMT") == 0) {
        tm.tm_year -= 1900;
    } else if (std::sscanf(text.c_str(), "%15[A-Za-z], %2d-%3s-%2d %2d:%2d:%2d %3s%n", day, &tm.tm_mday, month,
                           &tm.tm_year, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, zone, &consumed) == 8 &&
               static_cast<size_t>(consumed) == text.size() && std::strcmp(zone, "GMT") == 0) {
        // RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT"; two-digit years are 19xx or 20xx.
        if (tm.tm_year < 70) tm.tm_year += 100;
    } else if (std::sscanf(text.c_str(), "%3s %3s %2d %2d:%2d:%2d %4d%n", day, month, &tm.tm_mday, &tm.tm_hour,
                           &tm.tm_min, &tm.tm_sec, &tm.tm_year, &consumed) == 7 &&
               static_cast<size_t>(consumed) == text.size()) {
        // asctime: "Sun Nov  6 08:49:37 1994"
        tm.tm_year -= 1900;
    } else {
        return std::nullopt;
    }
    tm.tm_mon = monthIndex(month);
    if (tm.tm_mon < 0 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60) {
        return std::nullopt;
    }
    return static_cast<int64_t>(timegm(&tm));
}

}  // namespace fourfold

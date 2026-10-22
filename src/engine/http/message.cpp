#include "http/message.h"

#include <algorithm>
#include <cctype>

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

}  // namespace fourfold

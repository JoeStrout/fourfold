#include "cgi/cgi_request.h"

#include <cctype>
#include <cstdlib>

namespace fourfold {

namespace {

std::optional<std::string> var(const CgiVariables& vars, const char* name) {
    auto it = vars.find(name);
    if (it == vars.end()) return std::nullopt;
    return it->second;
}

// HTTP_USER_AGENT → User-Agent.
std::string headerName(const std::string& variable, size_t from) {
    std::string name;
    bool upper = true;
    for (size_t i = from; i < variable.size(); i++) {
        char c = variable[i];
        if (c == '_') {
            name += '-';
            upper = true;
        } else {
            name += upper ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            upper = false;
        }
    }
    return name;
}

bool isMultipart(const CgiVariables& vars) {
    std::string type = var(vars, "CONTENT_TYPE").value_or("");
    std::string lower;
    for (char c : type) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.rfind("multipart/form-data", 0) == 0;
}

}  // namespace

std::optional<size_t> cgiBodyLength(const CgiVariables& vars, const CgiLimits& limits, bool* tooLarge) {
    *tooLarge = false;
    std::string text = var(vars, "CONTENT_LENGTH").value_or("");
    if (text.empty()) return size_t(0);
    if (text.size() > 18) {
        *tooLarge = true;
        return std::nullopt;
    }
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    size_t length = static_cast<size_t>(std::strtoull(text.c_str(), nullptr, 10));
    size_t limit = isMultipart(vars) ? limits.maxUploadBodyBytes : limits.maxBodyBytes;
    if (length > limit) {
        *tooLarge = true;
        return std::nullopt;
    }
    return length;
}

HttpRequest cgiRequest(const CgiVariables& vars, std::string body) {
    HttpRequest r;
    r.method = var(vars, "REQUEST_METHOD").value_or("GET");
    if (auto uri = var(vars, "REQUEST_URI"); uri && !uri->empty()) {
        r.target = *uri;
    } else {
        r.target = var(vars, "PATH_INFO").value_or("/");
        if (r.target.empty()) r.target = "/";
        std::string query = var(vars, "QUERY_STRING").value_or("");
        if (!query.empty()) r.target += "?" + query;
    }
    std::string protocol = var(vars, "SERVER_PROTOCOL").value_or("HTTP/1.1");
    if (protocol == "HTTP/1.0") r.versionMinor = 0;
    if (protocol == "HTTP/2" || protocol == "HTTP/2.0") {
        r.versionMajor = 2;
        r.versionMinor = 0;
    }
    r.peerAddress = var(vars, "REMOTE_ADDR").value_or("");
    for (const auto& [name, value] : vars) {
        if (name.rfind("HTTP_", 0) == 0 && name.size() > 5) r.headers.add(headerName(name, 5), value);
    }
    // cgi-setup's .htaccess passes Authorization along (Apache doesn't), and
    // its rewrite renames it.
    if (!r.headers.has("Authorization")) {
        if (auto auth = var(vars, "REDIRECT_HTTP_AUTHORIZATION"); auth && !auth->empty()) {
            r.headers.add("Authorization", *auth);
        }
    }
    if (r.headers.has("Authorization") && r.headers.get("Authorization")->empty()) r.headers.remove("Authorization");
    if (auto type = var(vars, "CONTENT_TYPE"); type && !type->empty()) r.headers.add("Content-Type", *type);
    if (!body.empty()) r.headers.add("Content-Length", std::to_string(body.size()));
    r.body = std::move(body);
    return r;
}

bool cgiHttps(const CgiVariables& vars) {
    std::string https = var(vars, "HTTPS").value_or("");
    for (char& c : https) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return https == "on" || https == "1";
}

std::string cgiResponse(const HttpResponse& response) {
    std::string out = "Status: " + std::to_string(response.status) + " " + statusReason(response.status) + "\r\n";
    for (const auto& field : response.headers) {
        if (equalsIgnoreCase(field.name, "Content-Length")) continue;
        out += field.name + ": " + field.value + "\r\n";
    }
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n\r\n";
    out += response.body;
    return out;
}

}  // namespace fourfold

// A tiny HTTP client for end-to-end tests: one request per connection, and
// a cookie jar, as a browser keeps one. Free of doctest macros, so it can be
// used from many threads at once.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <map>
#include <string>
#include <thread>

namespace fourfold::test {

// One request over a fresh connection; the whole response, or "" if the
// connection failed.
inline std::string rawFetch(uint16_t port, const std::string& request) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    // The macOS sandbox sometimes refuses the first loopback connections.
    for (int tries = 0; connected < 0 && errno == EPERM && tries < 5; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    }
    if (connected < 0) {
        ::close(fd);
        return "";
    }
    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);
    return response;
}

inline std::string textBetween(const std::string& s, const std::string& before, const std::string& after) {
    size_t at = s.find(before);
    if (at == std::string::npos) return "";
    at += before.size();
    return s.substr(at, s.find(after, at) - at);
}

// A browser's cookies, and its CSRF token (from the last page that had one).
class Browser {
public:
    explicit Browser(uint16_t port) : port_(port) {}

    std::string get(const std::string& target) { return send("GET", target, ""); }

    // A form post, with the CSRF token a page gave.
    std::string post(const std::string& target, const std::string& body, bool htmx = false) {
        return send("POST", target, body, htmx);
    }

    std::string send(const std::string& method, const std::string& target, const std::string& body,
                     bool htmx = false) {
        std::string request = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
        std::string cookies;
        for (const auto& [name, value] : jar_) cookies += (cookies.empty() ? "" : "; ") + name + "=" + value;
        if (!cookies.empty()) request += "Cookie: " + cookies + "\r\n";
        if (!token_.empty()) request += "X-CSRF-Token: " + token_ + "\r\n";
        if (htmx) request += "HX-Request: true\r\n";
        if (method == "POST") {
            request += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n";
        }
        request += "\r\n" + body;
        std::string response = rawFetch(port_, request);
        remember(response);
        return response;
    }

    // Forget everything, as a new browser.
    void clear() {
        jar_.clear();
        token_.clear();
    }
    const std::map<std::string, std::string>& cookies() const { return jar_; }

private:
    void remember(const std::string& response) {
        size_t headersEnd = response.find("\r\n\r\n");
        std::string headers = response.substr(0, headersEnd);
        for (size_t at = headers.find("\r\nSet-Cookie: "); at != std::string::npos;
             at = headers.find("\r\nSet-Cookie: ", at + 1)) {
            size_t start = at + 14;
            std::string pair = headers.substr(start, headers.find(';', start) - start);
            size_t eq = pair.find('=');
            jar_[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        std::string token = textBetween(response, "X-CSRF-Token&quot;: &quot;", "&quot;");
        if (token.empty()) token = textBetween(response, "name=\"_csrf\" value=\"", "\"");
        if (!token.empty()) token_ = token;
    }

    uint16_t port_;
    std::map<std::string, std::string> jar_;
    std::string token_;
};

// The body of a response.
inline std::string bodyOf(const std::string& response) {
    size_t at = response.find("\r\n\r\n");
    return at == std::string::npos ? "" : response.substr(at + 4);
}

}  // namespace fourfold::test

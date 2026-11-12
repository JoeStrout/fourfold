// A fake HTTP server on 127.0.0.1, for tests of outbound requests: it
// answers each request with whatever `respond` returns (a whole HTTP
// response, as bytes), and records the requests it got. Header-only.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fourfold::test {

class FakeHttp {
public:
    struct Request {
        std::string head;   // the request line and headers
        std::string body;
        std::string target() const { return head.substr(head.find(' ') + 1, head.find(' ', head.find(' ') + 1) - head.find(' ') - 1); }
        std::string method() const { return head.substr(0, head.find(' ')); }
    };
    using Responder = std::function<std::string(const Request&)>;

    explicit FakeHttp(Responder respond) : respond_(std::move(respond)) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        listen(fd_, 16);
        socklen_t len = sizeof addr;
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeHttp() {
        stop_ = true;
        thread_.join();
        close(fd_);
    }

    int port() const { return port_; }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    // A response with this status, body and extra header lines ("X: y\r\n").
    static std::string response(int status, const std::string& body, const std::string& headers = "") {
        return "HTTP/1.1 " + std::to_string(status) + " X\r\n" + headers +
               "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    }

private:
    void serve() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            int c = accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            handle(c);
            close(c);
        }
    }

    void handle(int c) {
        std::string buffer;
        size_t end = std::string::npos;
        while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (!readMore(c, buffer)) return;
        }
        Request r;
        r.head = buffer.substr(0, end + 2);
        buffer.erase(0, end + 4);
        size_t length = 0;
        std::string lower = r.head;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        size_t at = lower.find("\r\ncontent-length: ");
        if (at != std::string::npos) length = std::strtoul(lower.c_str() + at + 18, nullptr, 10);
        while (buffer.size() < length) {
            if (!readMore(c, buffer)) break;
        }
        r.body = buffer.substr(0, length);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(r);
        }
        std::string out = respond_(r);
        size_t sent = 0;
        while (sent < out.size()) {
            ssize_t n = ::send(c, out.data() + sent, out.size() - sent, 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    static bool readMore(int c, std::string& buffer) {
        pollfd p{c, POLLIN, 0};
        if (poll(&p, 1, 2000) <= 0) return false;
        char chunk[4096];
        ssize_t n = ::recv(c, chunk, sizeof chunk, 0);
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(n));
        return true;
    }

    Responder respond_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<Request> requests_;
};

}  // namespace fourfold::test

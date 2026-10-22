#include "platform/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fourfold {

namespace {

Error errnoError(const char* what) {
    int e = errno;
    ErrorCode code = (e == EACCES || e == EPERM) ? ErrorCode::Denied : ErrorCode::IO;
    return makeError(code, std::string(what) + ": " + std::strerror(e));
}

// Stops writes to a closed peer from raising SIGPIPE, where the OS supports
// that per socket (macOS). Elsewhere, writeSome passes MSG_NOSIGNAL.
void disableSigPipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

std::string addressToString(const sockaddr* addr, socklen_t len) {
    char host[NI_MAXHOST];
    if (getnameinfo(addr, len, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0) return "";
    return host;
}

}  // namespace

Status setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return errnoError("fcntl");
    return {};
}

Result<Fd> listenTcp(const std::string& host, uint16_t port, int backlog) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    addrinfo* info = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &info) != 0 || !info) {
        return makeError(ErrorCode::Invalid, "not a numeric address: " + host);
    }
    Fd fd(socket(info->ai_family, info->ai_socktype, info->ai_protocol));
    if (!fd.valid()) {
        freeaddrinfo(info);
        return errnoError("socket");
    }
    int on = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    int bound = bind(fd.get(), info->ai_addr, info->ai_addrlen);
    freeaddrinfo(info);
    if (bound < 0) return errnoError("bind");
    if (listen(fd.get(), backlog) < 0) return errnoError("listen");
    if (Status s = setNonBlocking(fd.get()); !s) return s.error();
    return fd;
}

Result<uint16_t> localPort(int fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return errnoError("getsockname");
    }
    if (addr.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    }
    return makeError(ErrorCode::Invalid, "not an IP socket");
}

Result<std::optional<AcceptedSocket>> acceptConnection(int listenFd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    for (;;) {
        int fd = accept(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd >= 0) {
            AcceptedSocket accepted{Fd(fd), addressToString(reinterpret_cast<sockaddr*>(&addr), len)};
            if (Status s = setNonBlocking(fd); !s) return s.error();
            disableSigPipe(fd);
            return std::optional<AcceptedSocket>(std::move(accepted));
        }
        if (errno == EINTR) continue;
        // ECONNABORTED: the client gave up while queued; nothing to accept.
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
            return std::optional<AcceptedSocket>();
        }
        return errnoError("accept");
    }
}

Result<std::pair<Fd, Fd>> makeSocketPair() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return errnoError("socketpair");
    Fd a(fds[0]), b(fds[1]);
    for (int fd : fds) {
        if (Status s = setNonBlocking(fd); !s) return s.error();
        disableSigPipe(fd);
    }
    return std::make_pair(std::move(a), std::move(b));
}

Result<size_t> readSome(int fd, char* buffer, size_t size) {
    for (;;) {
        ssize_t n = ::read(fd, buffer, size);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return makeError(ErrorCode::WouldBlock, "");
        return errnoError("read");
    }
}

Result<size_t> writeSome(int fd, const char* data, size_t size) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    for (;;) {
        ssize_t n = ::send(fd, data, size, kFlags);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return makeError(ErrorCode::WouldBlock, "");
        return errnoError("send");
    }
}

}  // namespace fourfold

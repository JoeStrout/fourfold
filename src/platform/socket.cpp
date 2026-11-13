#include "platform/socket.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
    if (addr->sa_family == AF_UNIX) return kUnixPeer;
    char host[NI_MAXHOST];
    if (getnameinfo(addr, len, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0) return "";
    return host;
}

// A Unix-domain address for `path`, or an error if it's too long.
Result<sockaddr_un> unixAddress(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof addr.sun_path) {
        return makeError(ErrorCode::Invalid, "not a usable socket path (empty, or too long): " + path);
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    return addr;
}

}  // namespace

const char* const kUnixPeer = "unix";

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
    if (info->ai_family == AF_INET6 && host == "::") {
        // "::" means every address, IPv4 too, whatever the system's default.
        int off = 0;
        setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    }
    int bound = bind(fd.get(), info->ai_addr, info->ai_addrlen);
    freeaddrinfo(info);
    if (bound < 0) return errnoError("bind");
    if (listen(fd.get(), backlog) < 0) return errnoError("listen");
    if (Status s = setNonBlocking(fd.get()); !s) return s.error();
    return fd;
}

Result<Fd> listenUnix(const std::string& path, unsigned mode, int backlog) {
    auto addr = unixAddress(path);
    if (!addr) return addr.error();
    // Only a socket file is replaced: a stale one, from a server that died.
    struct stat info;
    if (lstat(path.c_str(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode)) return makeError(ErrorCode::Conflict, path + ": exists, and isn't a socket");
        if (::unlink(path.c_str()) < 0) return errnoError("unlink");
    }
    Fd fd(socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) return errnoError("socket");
    fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    // Created with no access, then opened up to `mode`, so there's no moment
    // when others could connect.
    mode_t old = umask(0777);
    int bound = bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr.value()), sizeof(sockaddr_un));
    int e = errno;
    umask(old);
    errno = e;
    if (bound < 0) return errnoError("bind");
    if (chmod(path.c_str(), static_cast<mode_t>(mode & 0777)) < 0) return errnoError("chmod");
    if (::listen(fd.get(), backlog) < 0) return errnoError("listen");
    if (Status s = setNonBlocking(fd.get()); !s) return s.error();
    return fd;
}

Result<Fd> connectUnix(const std::string& path) {
    auto addr = unixAddress(path);
    if (!addr) return addr.error();
    Fd fd(socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) return errnoError("socket");
    fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    if (Status s = setNonBlocking(fd.get()); !s) return s.error();
    disableSigPipe(fd.get());
    for (;;) {
        if (connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr.value()), sizeof(sockaddr_un)) == 0) return fd;
        if (errno == EINTR) continue;
        // A local connect either completes at once or fails; EAGAIN (Linux)
        // means the listener's queue is full.
        if (errno == EAGAIN) return makeError(ErrorCode::WouldBlock, "connect: " + path + " is too busy");
        return errnoError("connect");
    }
}

std::string unixSocketPath(int fd) {
    sockaddr_un addr{};
    socklen_t len = sizeof addr;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0 || addr.sun_family != AF_UNIX) return "";
    size_t n = len > offsetof(sockaddr_un, sun_path) ? len - offsetof(sockaddr_un, sun_path) : 0;
    return std::string(addr.sun_path, strnlen(addr.sun_path, n));
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

size_t connectionsForOpenFiles() {
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) != 0) return 1024;
    rlim_t want = r.rlim_max == RLIM_INFINITY ? 65536 : std::min<rlim_t>(r.rlim_max, 65536);
    if (r.rlim_cur < want) {
        struct rlimit raised = r;
        raised.rlim_cur = want;
        if (setrlimit(RLIMIT_NOFILE, &raised) == 0) r = raised;
    }
    rlim_t files = r.rlim_cur == RLIM_INFINITY ? 65536 : r.rlim_cur;
    return files > 512 ? static_cast<size_t>(files - 256) : 256;
}

std::vector<InheritedSocket> takeSystemdSockets() {
    std::vector<InheritedSocket> out;
    // Copied: unsetenv frees them.
    auto var = [](const char* name) {
        const char* v = getenv(name);
        return v ? std::optional<std::string>(v) : std::nullopt;
    };
    auto pid = var("LISTEN_PID"), count = var("LISTEN_FDS"), names = var("LISTEN_FDNAMES");
    bool ours = pid && count && std::to_string(getpid()) == *pid;
    std::vector<std::string> nameList;
    if (names) {
        std::string all = *names;
        size_t start = 0;
        while (start <= all.size()) {
            size_t colon = all.find(':', start);
            if (colon == std::string::npos) colon = all.size();
            nameList.push_back(all.substr(start, colon - start));
            start = colon + 1;
        }
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    if (!ours) return out;
    int n = atoi(count->c_str());
    for (int i = 0; i < n && i < 64; i++) {
        int fd = 3 + i;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        out.push_back({i < static_cast<int>(nameList.size()) ? nameList[i] : std::string(), Fd(fd)});
    }
    return out;
}

}  // namespace fourfold

#include "dropboxwriter/dropbox_writer.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "platform/socket.h"

namespace fourfold {

namespace {

void putU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

uint32_t getU32(const char* p) {
    auto b = reinterpret_cast<const unsigned char*>(p);
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}

Error osError(const std::string& what) {
    int e = errno;
    return makeError(e == EACCES || e == EPERM ? ErrorCode::Denied : ErrorCode::IO,
                     "dropbox writer: " + what + ": " + std::strerror(e));
}

constexpr size_t kHeader = 8;
constexpr size_t kReplyHeader = 5;
constexpr int kMaxCode = static_cast<int>(ErrorCode::Internal);

}  // namespace

std::string encodeDropboxRequest(std::string_view name, std::string_view text) {
    std::string out;
    out.reserve(kHeader + name.size() + text.size());
    putU32(out, static_cast<uint32_t>(name.size()));
    putU32(out, static_cast<uint32_t>(text.size()));
    out.append(name);
    out.append(text);
    return out;
}

std::string encodeDropboxReply(const Status& status) {
    std::string out;
    out.push_back(status ? char(0) : static_cast<char>(static_cast<int>(status.error().code) + 1));
    std::string message = status ? std::string() : status.error().message.substr(0, 1024);
    putU32(out, static_cast<uint32_t>(message.size()));
    out += message;
    return out;
}

// --- The engine's side ---

Status DropboxClient::connect() {
    fd_.reset();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.empty() || path_.size() >= sizeof addr.sun_path) {
        return makeError(ErrorCode::Invalid, "dropbox writer: not a usable socket path: " + path_);
    }
    std::memcpy(addr.sun_path, path_.data(), path_.size());
    Fd fd(socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) return osError("socket");
    fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    timeval tv{timeoutMs_ / 1000, (timeoutMs_ % 1000) * 1000};
    setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    for (;;) {
        if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0) break;
        if (errno == EINTR) continue;
        return osError("can't connect to " + path_);
    }
    fd_ = std::move(fd);
    return {};
}

Status DropboxClient::exchange(const std::string& request) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = ::send(fd_.get(), request.data() + sent, request.size() - sent, kFlags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return osError("sending");
        sent += static_cast<size_t>(n);
    }
    auto readAll = [&](char* buffer, size_t size) -> Status {
        size_t got = 0;
        while (got < size) {
            ssize_t n = ::recv(fd_.get(), buffer + got, size - got, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n == 0) return makeError(ErrorCode::IO, "dropbox writer: it closed the connection");
            if (n < 0) return osError("waiting for its answer");
            got += static_cast<size_t>(n);
        }
        return {};
    };
    char header[kReplyHeader];
    if (Status s = readAll(header, sizeof header); !s) return s;
    uint32_t length = getU32(header + 1);
    if (length > 1024) return makeError(ErrorCode::IO, "dropbox writer: a malformed answer");
    std::string message(length, '\0');
    if (Status s = readAll(message.data(), length); !s) return s;
    int status = static_cast<unsigned char>(header[0]);
    if (status == 0) return {};
    if (status - 1 > kMaxCode) return makeError(ErrorCode::IO, "dropbox writer: a malformed answer");
    // A refusal from the writer is the append's own error ("not found", "too
    // large"), not the connection's.
    return makeError(static_cast<ErrorCode>(status - 1), message);
}

Status DropboxClient::append(const RelPath& path, std::string_view bytes) {
    if (bytes.size() > DropboxProtocolLimits::kMaxText) {
        return makeError(ErrorCode::TooLarge, "dropbox: a write is limited to " +
                                                  std::to_string(DropboxProtocolLimits::kMaxText / 1024) + " KB");
    }
    std::string request = encodeDropboxRequest(path.str(), bytes);
    std::lock_guard<std::mutex> lock(mutex_);
    // Once more on a fresh connection, if the old one has gone (the writer
    // restarted); not after the writer has answered.
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!fd_.valid()) {
            if (Status s = connect(); !s) return s;
        }
        Status s = exchange(request);
        if (s || s.error().message.compare(0, 15, "dropbox writer:") != 0) return s;
        fd_.reset();
        if (attempt == 1) return s;
    }
    return {};
}

// --- The writer's side ---

DropboxWriter::DropboxWriter(EventLoop& loop, Dropbox dropbox) : loop_(loop), dropbox_(std::move(dropbox)) {}

DropboxWriter::~DropboxWriter() {
    if (listener_.valid()) loop_.unwatch(listener_.get());
    for (auto& [fd, c] : connections_) loop_.unwatch(fd);
}

Status DropboxWriter::adopt(Fd listening) {
    if (Status s = setNonBlocking(listening.get()); !s) return s;
    if (Status s = loop_.watch(listening.get(), io::kReadable, [this](uint32_t) { onAcceptable(); }); !s) return s;
    listener_ = std::move(listening);
    return {};
}

void DropboxWriter::onAcceptable() {
    for (;;) {
        auto accepted = acceptConnection(listener_.get());
        if (!accepted || !accepted.value()) return;
        addConnection(std::move(accepted.value()->fd));
    }
}

void DropboxWriter::addConnection(Fd fd) {
    int raw = fd.get();
    auto c = std::make_unique<Connection>();
    c->fd = std::move(fd);
    if (!loop_.watch(raw, io::kReadable, [this, raw](uint32_t events) { onEvent(raw, events); })) return;
    connections_[raw] = std::move(c);
}

void DropboxWriter::onEvent(int fd, uint32_t events) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    Connection& c = *it->second;
    if (events & io::kWritable) {
        flush(c);
        if (connections_.find(fd) == connections_.end()) return;
    }
    if (!(events & io::kReadable) || c.closing) return;
    char buffer[65536];
    for (;;) {
        auto n = readSome(fd, buffer, sizeof buffer);
        if (!n) {
            if (n.error().code == ErrorCode::WouldBlock) break;
            close(fd);
            return;
        }
        if (n.value() == 0) {
            close(fd);
            return;
        }
        c.in.append(buffer, n.value());
        if (c.in.size() >= kHeader + DropboxProtocolLimits::kMaxName + DropboxProtocolLimits::kMaxText) break;
    }
    process(c);
    flush(c);
}

void DropboxWriter::process(Connection& c) {
    size_t pos = 0;
    while (!c.closing && c.in.size() - pos >= kHeader) {
        uint32_t nameLength = getU32(c.in.data() + pos);
        uint32_t textLength = getU32(c.in.data() + pos + 4);
        if (nameLength > DropboxProtocolLimits::kMaxName || textLength > DropboxProtocolLimits::kMaxText) {
            stats_.refused++;
            c.out += encodeDropboxReply(makeError(ErrorCode::TooLarge, "dropbox: a name or write over the limit"));
            c.closing = true;
            break;
        }
        if (c.in.size() - pos < kHeader + nameLength + textLength) break;
        std::string name = c.in.substr(pos + kHeader, nameLength);
        std::string_view text(c.in.data() + pos + kHeader + nameLength, textLength);
        pos += kHeader + nameLength + textLength;
        auto path = RelPath::parse(name);
        Status s;
        if (!path || path.value().isRoot()) {
            s = makeError(ErrorCode::Invalid, "dropbox: not a file name: " + name.substr(0, 100));
        } else {
            s = dropbox_.append(path.value(), text);
        }
        if (s) {
            stats_.appended++;
        } else {
            stats_.refused++;
        }
        c.out += encodeDropboxReply(s);
    }
    c.in.erase(0, pos);
}

void DropboxWriter::flush(Connection& c) {
    int fd = c.fd.get();
    while (!c.out.empty()) {
        auto n = writeSome(fd, c.out.data(), c.out.size());
        if (!n) {
            if (n.error().code == ErrorCode::WouldBlock) break;
            close(fd);
            return;
        }
        c.out.erase(0, n.value());
    }
    if (c.out.empty() && c.closing) {
        close(fd);
        return;
    }
    (void)loop_.setInterest(fd, c.out.empty() ? io::kReadable : io::kReadable | io::kWritable);
}

void DropboxWriter::close(int fd) {
    loop_.unwatch(fd);
    connections_.erase(fd);
}

}  // namespace fourfold

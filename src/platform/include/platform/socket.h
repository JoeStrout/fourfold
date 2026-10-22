// Thin wrappers over POSIX sockets: the few socket operations the hosts
// need. All sockets made here are non-blocking, and writing to a closed peer
// returns an error instead of raising SIGPIPE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>

#include "base/fd.h"
#include "base/result.h"

namespace fourfold {

Status setNonBlocking(int fd);

// A listening TCP socket on `host` (a numeric IPv4 or IPv6 address) and
// `port`; port 0 picks a free port (see localPort).
Result<Fd> listenTcp(const std::string& host, uint16_t port, int backlog = 128);

// The port a socket is bound to.
Result<uint16_t> localPort(int fd);

struct AcceptedSocket {
    Fd fd;
    std::string peerAddress;   // numeric, e.g. "127.0.0.1" or "::1"
};

// Accepts one pending connection, or returns nullopt if none is waiting.
Result<std::optional<AcceptedSocket>> acceptConnection(int listenFd);

// A connected pair of Unix-domain stream sockets (for tests and wakeups).
Result<std::pair<Fd, Fd>> makeSocketPair();

// Reads or writes as much as possible without blocking. Return the byte
// count; 0 from readSome means the peer closed. When nothing can be read or
// written yet, they return an error with code WouldBlock.
Result<size_t> readSome(int fd, char* buffer, size_t size);
Result<size_t> writeSome(int fd, const char* data, size_t size);

}  // namespace fourfold

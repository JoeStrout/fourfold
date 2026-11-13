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
#include <vector>

#include "base/fd.h"
#include "base/result.h"

namespace fourfold {

Status setNonBlocking(int fd);

// A listening TCP socket on `host` (a numeric IPv4 or IPv6 address) and
// `port`; port 0 picks a free port (see localPort).
Result<Fd> listenTcp(const std::string& host, uint16_t port, int backlog = 128);

// The port a socket is bound to.
Result<uint16_t> localPort(int fd);

// What acceptConnection gives as the peer address of a Unix-domain socket's
// connection. Who may connect is up to the socket file's permissions, so the
// engine trusts such a peer as a proxy (overview §8, several apps on one
// server).
extern const char* const kUnixPeer;

// A listening Unix-domain stream socket at `path`, with the socket file's
// permission bits set to `mode` (e.g. 0660: the owner's and group's to
// connect to). A stale socket file left at `path` is replaced; anything else
// there is an error.
Result<Fd> listenUnix(const std::string& path, unsigned mode = 0660, int backlog = 128);

// Connects to the Unix-domain socket at `path`, returning a non-blocking
// socket. A local connect completes at once: it fails if nothing is
// listening there (or the socket file's permissions say no), and with
// WouldBlock if the listener's queue is full.
Result<Fd> connectUnix(const std::string& path);

// The path of a Unix-domain socket's address ("" if it isn't one).
std::string unixSocketPath(int fd);

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

// Connections a server may hold: what the open-file limit allows (raised to
// its hard limit, up to 65536), less room for the process's other files.
size_t connectionsForOpenFiles();

// Sockets passed by systemd's socket activation (sd_listen_fds(3)):
// LISTEN_FDS sockets from fd 3, if LISTEN_PID is this process, each with its
// name from LISTEN_FDNAMES (FileDescriptorName=; "" if none). The variables
// are then removed, so child processes don't take them too. Each is made
// close-on-exec.
struct InheritedSocket {
    std::string name;
    Fd fd;
};
std::vector<InheritedSocket> takeSystemdSockets();

}  // namespace fourfold

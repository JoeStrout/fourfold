// The dropbox writer (overview §1, OS-level enforcement): on a server with
// an OS user per app, a small process running as a second user owns the
// app's dropbox/, so the engine (and app code, if it ever got out of the
// sandbox) can add lines but never read, truncate, rename or remove them.
// The engine sends each append over a Unix-domain socket, which only the
// engine's user may connect to.
//
// The protocol, one exchange per append, in order on one connection:
//
//   request:  u32 name length, u32 text length (big-endian), name, text
//   reply:    u8 status (0: done; otherwise an ErrorCode + 1), u32 message
//             length, message
//
// The name is checked as the engine checks it (a RelPath, not the root);
// the writer appends through the same partition code as before, so links
// aren't followed and dot-names can't be named. A request over the limits
// is refused, and the connection closed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/fd.h"
#include "base/result.h"
#include "partition/data_dir.h"
#include "platform/event_loop.h"

namespace fourfold {

struct DropboxProtocolLimits {
    static constexpr size_t kMaxName = 1024;
    static constexpr size_t kMaxText = 1024 * 1024;
};

// The engine's side: a DropboxSink that sends each append to the writer
// listening at `socketPath`, and waits for its answer. Connects on first
// use, and again after a failure (one retry per append). Blocking, with a
// timeout; called from worker threads, one append at a time.
class DropboxClient : public DropboxSink {
public:
    explicit DropboxClient(std::string socketPath, int timeoutMs = 5000)
        : path_(std::move(socketPath)), timeoutMs_(timeoutMs) {}

    Status append(const RelPath& path, std::string_view bytes) override;

    const std::string& socketPath() const { return path_; }

private:
    Status connect();
    Status exchange(const std::string& request);

    std::string path_;
    int timeoutMs_;
    std::mutex mutex_;
    Fd fd_;
};

// The writer's side: accepts the engine's connections on a listening
// socket, and appends what they send to `dropbox`, on `loop`'s thread.
class DropboxWriter {
public:
    DropboxWriter(EventLoop& loop, Dropbox dropbox);
    ~DropboxWriter();

    DropboxWriter(const DropboxWriter&) = delete;
    DropboxWriter& operator=(const DropboxWriter&) = delete;

    // Accepts connections on `listening` (from listenUnix, or systemd).
    Status adopt(Fd listening);
    // Serves one connected socket (tests use a socket pair).
    void addConnection(Fd fd);

    struct Stats {
        uint64_t appended = 0;   // lines written
        uint64_t refused = 0;    // bad names, failed writes, protocol errors
    };
    Stats stats() const { return stats_; }
    size_t connectionCount() const { return connections_.size(); }

private:
    struct Connection {
        Fd fd;
        std::string in;
        std::string out;
        bool closing = false;   // close once `out` is sent
    };
    void onAcceptable();
    void onEvent(int fd, uint32_t events);
    // Handles every complete request in the connection's buffer.
    void process(Connection& c);
    void flush(Connection& c);
    void close(int fd);

    EventLoop& loop_;
    Dropbox dropbox_;
    Fd listener_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    Stats stats_;
};

// For the protocol's tests: a request, and a reply.
std::string encodeDropboxRequest(std::string_view name, std::string_view text);
std::string encodeDropboxReply(const Status& status);

}  // namespace fourfold

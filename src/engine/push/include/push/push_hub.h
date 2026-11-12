// PushHub: push channels and their subscribers (overview §7, Real-time push
// via Server-Sent Events; Arch §5 push).
//
// A page subscribes to /live/<name> (the engine checks access first); the
// hub keeps the open streams by channel, and writes each published message
// to every subscriber of its channel as a server-sent event. No VM lives
// beyond its request: the hub holds sockets, and messages are plain text.
//
// The streams belong to the transport's loop, so subscribe() is called
// there; publish() may be called from any thread (it posts to the loop).
#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "http/message.h"
#include "platform/event_loop.h"

namespace fourfold {

class PushHub : public std::enable_shared_from_this<PushHub> {
public:
    struct Limits {
        // Open subscriptions, in all; another is refused (503).
        size_t maxSubscribers = 1000;
        // A comment line this often keeps proxies from closing idle streams,
        // and finds closed ones.
        std::chrono::milliseconds heartbeat{15'000};
    };

    // `loop` is the transport's; it must outlive the hub.
    static std::shared_ptr<PushHub> create(EventLoop& loop, Limits limits);
    static std::shared_ptr<PushHub> create(EventLoop& loop) { return create(loop, Limits{}); }

    // Whether another subscriber may join now. Any thread.
    bool full() const;
    // On the loop's thread: adds a stream to `channel`. It hears every
    // message published to the channel from now on, until it closes.
    void subscribe(const std::string& channel, std::shared_ptr<StreamWriter> writer);
    // Any thread: sends `data` (HTML) as event `event` to every subscriber
    // of `channel`.
    void publish(const std::string& channel, const std::string& event, const std::string& data);

    // Subscribers now, by channel (for the dev tools and tests). Any thread.
    std::map<std::string, size_t> counts() const;
    size_t total() const;

private:
    PushHub(EventLoop& loop, Limits limits) : loop_(loop), limits_(limits) {}
    void startHeartbeat();
    void sweep();   // drops closed streams (loop thread)
    void updateCounts();

    EventLoop& loop_;
    Limits limits_;
    // Loop thread only.
    std::map<std::string, std::vector<std::shared_ptr<StreamWriter>>> channels_;
    bool heartbeatRunning_ = false;
    // A copy of the counts, for any thread.
    mutable std::mutex countsMutex_;
    std::map<std::string, size_t> counts_;
    size_t total_ = 0;
};

// One server-sent event: "event: <event>\n", then "data: <line>\n" for each
// line of `data` (without its final line break), then a blank line.
std::string pushEvent(const std::string& event, const std::string& data);

}  // namespace fourfold

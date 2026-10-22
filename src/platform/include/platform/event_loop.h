// EventLoop: a single-threaded loop that waits for file descriptors to become
// ready, runs timers, and runs tasks posted from other threads.
//
// Everything except post() and stop() must be called on the loop's own
// thread (the one calling run or runOnce). Callbacks run on that thread, one
// at a time, and may freely watch, unwatch, add or cancel timers, including
// for the watch or timer that is currently running.
//
// Uses kqueue on macOS and the BSDs, and epoll on Linux (Arch §3).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "base/result.h"
#include "platform/socket.h"

namespace fourfold {

// Readiness bits, for interests and for events.
namespace io {
constexpr uint32_t kReadable = 1;
constexpr uint32_t kWritable = 2;
}  // namespace io

class Poller;   // the OS-specific backend (private)

class EventLoop {
public:
    using Clock = std::chrono::steady_clock;
    using IoCallback = std::function<void(uint32_t events)>;
    using Task = std::function<void()>;
    using TimerId = uint64_t;

    static Result<std::unique_ptr<EventLoop>> create();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Calls `callback` when `fd` becomes ready for any of the `interest` bits.
    // Errors and hang-ups are reported as readable (and writable, if asked
    // for), so the next read or write sees them. Replaces any earlier watch
    // of the same fd. The loop doesn't own the fd; unwatch it before closing.
    Status watch(int fd, uint32_t interest, IoCallback callback);
    // Changes what an existing watch waits for. Interest 0 pauses it.
    Status setInterest(int fd, uint32_t interest);
    void unwatch(int fd);

    // Runs `task` once, after `delay`.
    TimerId addTimer(std::chrono::milliseconds delay, Task task);
    // Does nothing if the timer has already run or been cancelled.
    void cancelTimer(TimerId id);

    // Thread-safe: runs `task` on the loop thread soon.
    void post(Task task);

    // Runs until stop() is called.
    void run();
    // Waits up to `maxWait` (or less, if a timer is due) for events, then
    // handles everything that is ready. For tests and custom loops.
    void runOnce(std::chrono::milliseconds maxWait);
    // Thread-safe: makes run() return after the current iteration.
    void stop();

private:
    EventLoop(std::unique_ptr<Poller> poller, Fd wakeRead, Fd wakeWrite);

    struct Watch {
        uint32_t interest;
        uint32_t generation;   // distinguishes a re-watched fd from its old watch
        std::shared_ptr<IoCallback> callback;
    };

    void runDueTimers();
    void runPostedTasks();
    int msUntilNextTimer(std::chrono::milliseconds maxWait) const;

    std::unique_ptr<Poller> poller_;
    std::unordered_map<int, Watch> watches_;
    uint32_t nextGeneration_ = 1;

    std::map<std::pair<Clock::time_point, TimerId>, Task> timers_;
    std::unordered_map<TimerId, Clock::time_point> timerDeadlines_;
    TimerId nextTimerId_ = 1;

    Fd wakeRead_, wakeWrite_;   // a socket pair; a byte written wakes the loop
    std::mutex postedMutex_;
    std::vector<Task> posted_;
    std::atomic<bool> stopping_{false};
};

}  // namespace fourfold

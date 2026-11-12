// SchemaGate: the schema lock (overview §3, Applying migrations), in
// memory, for the standalone server.
//
// Every request holds it shared while it runs: enter() as it starts,
// leave() once its response is made. Migrating takes it exclusively:
// pause() stops admitting new requests, which wait in a queue instead of
// failing; waitForIdle() blocks until the requests already inside have
// finished (each is bounded by its budget); resume() lets the queue in,
// onto whatever version of the app is current by then. A request that
// waits longer than the limit given to pause() is turned away instead (a
// 503, "down for maintenance").
//
// Thread-safe. Callbacks run on whichever thread admits or turns away the
// request (the caller's, the resumer's, or the gate's timer thread), never
// under the gate's lock.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace fourfold {

class SchemaGate {
public:
    // Called once: true when the request may go ahead (it's inside, and
    // must leave() later), false if it waited too long (it isn't inside).
    using Admitted = std::function<void(bool admitted)>;

    SchemaGate() = default;
    virtual ~SchemaGate();
    SchemaGate(const SchemaGate&) = delete;
    SchemaGate& operator=(const SchemaGate&) = delete;

    // Virtual so CGI mode can hold a lock file instead (FileSchemaGate),
    // for `fourfold run-jobs`.
    virtual void enter(Admitted admitted);
    virtual void leave();

    // Stops admitting; requests that arrive wait up to `maxWait`.
    void pause(std::chrono::milliseconds maxWait);
    // Blocks until no request is inside (call after pause).
    void waitForIdle();
    // Admits everything waiting, and new requests again.
    void resume();

    struct Stats {
        size_t inside = 0;
        size_t waiting = 0;
        bool paused = false;
    };
    Stats stats() const;

private:
    struct Waiter {
        Admitted admitted;
        std::chrono::steady_clock::time_point deadline;
    };
    void timeouts();   // the timer thread, while paused

    mutable std::mutex mutex_;
    std::condition_variable idle_;    // inside_ reached 0
    std::condition_variable timer_;   // wakes the timer thread
    size_t inside_ = 0;
    bool paused_ = false;
    std::chrono::milliseconds maxWait_{0};
    std::deque<Waiter> waiting_;
    std::thread timerThread_;
};

}  // namespace fourfold

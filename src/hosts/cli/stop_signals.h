// SIGTERM and SIGINT, for a server: the first shuts it down gracefully, a
// second stops it at once. Construct before starting the server, so every
// thread it starts has the signals blocked, and only run()'s waiter takes
// them.
#pragma once

#include <atomic>
#include <csignal>
#include <functional>
#include <pthread.h>
#include <thread>

#include "platform/event_loop.h"

namespace fourfold::cli {

class StopSignals {
public:
    StopSignals() {
        // A client gone mid-write is an error to handle, not a reason to die.
        std::signal(SIGPIPE, SIG_IGN);
        sigemptyset(&set_);
        sigaddset(&set_, SIGTERM);
        sigaddset(&set_, SIGINT);
        pthread_sigmask(SIG_BLOCK, &set_, &previous_);
    }
    ~StopSignals() { pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;

    // Runs `loop` until it stops. On the first signal, `shutdown` runs on the
    // loop's thread (and should stop the loop once it's done); on a second,
    // the loop stops at once.
    void run(EventLoop& loop, std::function<void()> shutdown) {
        std::atomic<bool> finished{false};
        std::thread waiter([&] {
            int count = 0;
            for (;;) {
                int signal = 0;
                if (sigwait(&set_, &signal) != 0 || finished) return;
                if (count++ == 0) {
                    loop.post(shutdown);
                } else {
                    loop.stop();
                }
            }
        });
        loop.run();
        finished = true;
        pthread_kill(waiter.native_handle(), SIGTERM);   // wakes it, to see `finished`
        waiter.join();
    }

private:
    sigset_t set_, previous_;
};

}  // namespace fourfold::cli

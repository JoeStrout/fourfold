// Clock: the time, injectable, so timeouts and expiry can be tested without
// waiting (Arch §5 base).
#pragma once

#include <atomic>
#include <cstdint>

namespace fourfold {

class Clock {
public:
    virtual ~Clock() = default;
    // Wall-clock time: milliseconds since the Unix epoch. For times that are
    // stored (a session's creation, its last use) and compared later, maybe
    // by another process.
    virtual int64_t nowMillis() const = 0;
};

// The system's clock.
const Clock& systemClock();

// A clock that moves only when told to. Thread-safe.
class ManualClock : public Clock {
public:
    explicit ManualClock(int64_t startMillis = 1'700'000'000'000) : now_(startMillis) {}
    int64_t nowMillis() const override { return now_.load(); }
    void advanceMillis(int64_t ms) { now_ += ms; }
    void advanceSeconds(int64_t s) { now_ += s * 1000; }

private:
    std::atomic<int64_t> now_;
};

}  // namespace fourfold

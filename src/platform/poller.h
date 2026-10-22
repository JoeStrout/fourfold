// The OS-specific half of EventLoop: registering interest in file
// descriptors and waiting for readiness. One implementation is compiled per
// platform (poller_kqueue.cpp, poller_epoll.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "base/result.h"

namespace fourfold {

struct PollEvent {
    uint64_t token;    // as given to add()
    uint32_t events;   // io::kReadable | io::kWritable
};

class Poller {
public:
    virtual ~Poller() = default;

    virtual Status add(int fd, uint32_t interest, uint64_t token) = 0;
    virtual Status modify(int fd, uint32_t oldInterest, uint32_t newInterest, uint64_t token) = 0;
    virtual void remove(int fd, uint32_t interest) = 0;

    // Waits up to timeoutMs (-1: forever) and appends ready events to `out`.
    // An interrupted wait returns no events and no error.
    virtual Status wait(int timeoutMs, std::vector<PollEvent>& out) = 0;
};

Result<std::unique_ptr<Poller>> makePoller();

}  // namespace fourfold

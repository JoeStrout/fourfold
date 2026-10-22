#if defined(__linux__)

#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <unistd.h>

#include "platform/event_loop.h"
#include "poller.h"

namespace fourfold {

namespace {

Error epollError(const char* what) {
    return makeError(ErrorCode::IO, std::string(what) + ": " + std::strerror(errno));
}

uint32_t toEpoll(uint32_t interest) {
    uint32_t events = 0;
    if (interest & io::kReadable) events |= EPOLLIN | EPOLLRDHUP;
    if (interest & io::kWritable) events |= EPOLLOUT;
    return events;
}

class EpollPoller : public Poller {
public:
    explicit EpollPoller(int ep) : ep_(ep) {}
    ~EpollPoller() override { close(ep_); }

    Status add(int fd, uint32_t interest, uint64_t token) override {
        return control(EPOLL_CTL_ADD, fd, interest, token);
    }

    Status modify(int fd, uint32_t, uint32_t newInterest, uint64_t token) override {
        return control(EPOLL_CTL_MOD, fd, newInterest, token);
    }

    void remove(int fd, uint32_t) override {
        epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
    }

    Status wait(int timeoutMs, std::vector<PollEvent>& out) override {
        epoll_event events[64];
        int n = epoll_wait(ep_, events, 64, timeoutMs);
        if (n < 0) {
            if (errno == EINTR) return {};
            return epollError("epoll_wait");
        }
        for (int i = 0; i < n; i++) {
            uint32_t e = events[i].events;
            uint32_t bits = 0;
            if (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) bits |= io::kReadable;
            if (e & (EPOLLOUT | EPOLLHUP | EPOLLERR)) bits |= io::kWritable;
            out.push_back({events[i].data.u64, bits});
        }
        return {};
    }

private:
    Status control(int op, int fd, uint32_t interest, uint64_t token) {
        epoll_event ev{};
        ev.events = toEpoll(interest);
        ev.data.u64 = token;
        if (epoll_ctl(ep_, op, fd, &ev) < 0) return epollError("epoll_ctl");
        return {};
    }

    int ep_;
};

}  // namespace

Result<std::unique_ptr<Poller>> makePoller() {
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) return epollError("epoll_create1");
    return std::unique_ptr<Poller>(new EpollPoller(ep));
}

}  // namespace fourfold

#endif

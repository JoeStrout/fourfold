#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include <cerrno>
#include <cstring>
#include <sys/event.h>
#include <unistd.h>

#include "platform/event_loop.h"
#include "poller.h"

namespace fourfold {

namespace {

Error kqueueError(const char* what) {
    return makeError(ErrorCode::IO, std::string(what) + ": " + std::strerror(errno));
}

// kqueue tracks reading and writing as separate filters, each added or
// deleted on its own.
class KqueuePoller : public Poller {
public:
    explicit KqueuePoller(int kq) : kq_(kq) {}
    ~KqueuePoller() override { close(kq_); }

    Status add(int fd, uint32_t interest, uint64_t token) override {
        return modify(fd, 0, interest, token);
    }

    Status modify(int fd, uint32_t oldInterest, uint32_t newInterest, uint64_t token) override {
        struct kevent changes[2];
        int n = 0;
        auto change = [&](uint32_t bit, int16_t filter) {
            bool was = oldInterest & bit, want = newInterest & bit;
            if (was == want) return;
            EV_SET(&changes[n++], fd, filter, want ? EV_ADD : EV_DELETE, 0, 0,
                   reinterpret_cast<void*>(static_cast<uintptr_t>(token)));
        };
        change(io::kReadable, EVFILT_READ);
        change(io::kWritable, EVFILT_WRITE);
        if (n > 0 && kevent(kq_, changes, n, nullptr, 0, nullptr) < 0) return kqueueError("kevent");
        return {};
    }

    void remove(int fd, uint32_t interest) override {
        (void)modify(fd, interest, 0, 0);
    }

    Status wait(int timeoutMs, std::vector<PollEvent>& out) override {
        struct kevent events[64];
        timespec ts{}, *tsp = nullptr;
        if (timeoutMs >= 0) {
            ts.tv_sec = timeoutMs / 1000;
            ts.tv_nsec = static_cast<long>(timeoutMs % 1000) * 1000000;
            tsp = &ts;
        }
        int n = kevent(kq_, nullptr, 0, events, 64, tsp);
        if (n < 0) {
            if (errno == EINTR) return {};
            return kqueueError("kevent");
        }
        for (int i = 0; i < n; i++) {
            const struct kevent& ev = events[i];
            if (ev.flags & EV_ERROR) continue;   // a failed change; nothing to report
            uint32_t bits = ev.filter == EVFILT_READ ? io::kReadable : io::kWritable;
            out.push_back({static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev.udata)), bits});
        }
        return {};
    }

private:
    int kq_;
};

}  // namespace

Result<std::unique_ptr<Poller>> makePoller() {
    int kq = kqueue();
    if (kq < 0) return kqueueError("kqueue");
    return std::unique_ptr<Poller>(new KqueuePoller(kq));
}

}  // namespace fourfold

#endif

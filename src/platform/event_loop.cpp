#include "platform/event_loop.h"

#include <algorithm>

#include "poller.h"

namespace fourfold {

namespace {

uint64_t makeToken(int fd, uint32_t generation) {
    return (static_cast<uint64_t>(generation) << 32) | static_cast<uint32_t>(fd);
}

}  // namespace

Result<std::unique_ptr<EventLoop>> EventLoop::create() {
    auto poller = makePoller();
    if (!poller) return poller.error();
    auto pair = makeSocketPair();
    if (!pair) return pair.error();
    std::unique_ptr<EventLoop> loop(new EventLoop(std::move(poller).value(),
                                                  std::move(pair.value().first),
                                                  std::move(pair.value().second)));
    // The wake socket: drained here; posted tasks run after each wait.
    int wakeFd = loop->wakeRead_.get();
    Status s = loop->watch(wakeFd, io::kReadable, [wakeFd](uint32_t) {
        char buffer[256];
        for (;;) {
            auto n = readSome(wakeFd, buffer, sizeof buffer);
            if (!n || n.value() == 0) break;
        }
    });
    if (!s) return s.error();
    return loop;
}

EventLoop::EventLoop(std::unique_ptr<Poller> poller, Fd wakeRead, Fd wakeWrite)
    : poller_(std::move(poller)), wakeRead_(std::move(wakeRead)), wakeWrite_(std::move(wakeWrite)) {}

EventLoop::~EventLoop() {
    unwatch(wakeRead_.get());
}

Status EventLoop::watch(int fd, uint32_t interest, IoCallback callback) {
    unwatch(fd);
    uint32_t generation = nextGeneration_++;
    if (Status s = poller_->add(fd, interest, makeToken(fd, generation)); !s) return s;
    watches_[fd] = Watch{interest, generation, std::make_shared<IoCallback>(std::move(callback))};
    return {};
}

Status EventLoop::setInterest(int fd, uint32_t interest) {
    auto it = watches_.find(fd);
    if (it == watches_.end()) return makeError(ErrorCode::NotFound, "fd is not watched");
    Watch& w = it->second;
    if (w.interest == interest) return {};
    if (Status s = poller_->modify(fd, w.interest, interest, makeToken(fd, w.generation)); !s) return s;
    w.interest = interest;
    return {};
}

void EventLoop::unwatch(int fd) {
    auto it = watches_.find(fd);
    if (it == watches_.end()) return;
    poller_->remove(fd, it->second.interest);
    watches_.erase(it);
}

EventLoop::TimerId EventLoop::addTimer(std::chrono::milliseconds delay, Task task) {
    TimerId id = nextTimerId_++;
    Clock::time_point deadline = Clock::now() + delay;
    timers_.emplace(std::make_pair(deadline, id), std::move(task));
    timerDeadlines_[id] = deadline;
    return id;
}

void EventLoop::cancelTimer(TimerId id) {
    auto it = timerDeadlines_.find(id);
    if (it == timerDeadlines_.end()) return;
    timers_.erase({it->second, id});
    timerDeadlines_.erase(it);
}

void EventLoop::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(postedMutex_);
        posted_.push_back(std::move(task));
    }
    // If the socket buffer is full, the loop is already due to wake up.
    char byte = 0;
    (void)writeSome(wakeWrite_.get(), &byte, 1);
}

void EventLoop::stop() {
    stopping_ = true;
    char byte = 0;
    (void)writeSome(wakeWrite_.get(), &byte, 1);
}

void EventLoop::run() {
    stopping_ = false;
    while (!stopping_) runOnce(std::chrono::milliseconds(-1));
}

int EventLoop::msUntilNextTimer(std::chrono::milliseconds maxWait) const {
    if (timers_.empty()) return static_cast<int>(maxWait.count());
    auto untilTimer = std::chrono::duration_cast<std::chrono::milliseconds>(
        timers_.begin()->first.first - Clock::now());
    // Round up, so we don't wake a moment before the timer is due.
    long long ms = std::max<long long>(0, untilTimer.count() + 1);
    if (maxWait.count() >= 0) ms = std::min<long long>(ms, maxWait.count());
    return static_cast<int>(ms);
}

void EventLoop::runOnce(std::chrono::milliseconds maxWait) {
    std::vector<PollEvent> events;
    {
        bool hasPosted;
        {
            std::lock_guard<std::mutex> lock(postedMutex_);
            hasPosted = !posted_.empty();
        }
        int timeout = hasPosted ? 0 : msUntilNextTimer(maxWait);
        Status s = poller_->wait(timeout, events);
        (void)s;   // a failed wait (very rare) just means no events this round
    }

    for (const PollEvent& ev : events) {
        int fd = static_cast<int>(static_cast<uint32_t>(ev.token));
        uint32_t generation = static_cast<uint32_t>(ev.token >> 32);
        auto it = watches_.find(fd);
        // Skip events for watches removed or replaced earlier in this batch.
        if (it == watches_.end() || it->second.generation != generation) continue;
        uint32_t bits = ev.events & it->second.interest;
        if (!bits) continue;
        // Hold the callback, in case it unwatches its own fd.
        std::shared_ptr<IoCallback> callback = it->second.callback;
        (*callback)(bits);
    }

    runDueTimers();
    runPostedTasks();
}

void EventLoop::runDueTimers() {
    Clock::time_point now = Clock::now();
    while (!timers_.empty() && timers_.begin()->first.first <= now) {
        auto node = timers_.extract(timers_.begin());
        timerDeadlines_.erase(node.key().second);
        node.mapped()();
    }
}

void EventLoop::runPostedTasks() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(postedMutex_);
        tasks.swap(posted_);
    }
    for (Task& task : tasks) task();
}

}  // namespace fourfold

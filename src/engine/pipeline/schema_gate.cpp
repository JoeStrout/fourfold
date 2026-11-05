#include "pipeline/schema_gate.h"

#include <vector>

namespace fourfold {

SchemaGate::~SchemaGate() {
    resume();
}

void SchemaGate::enter(Admitted admitted) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused_) {
            waiting_.push_back({std::move(admitted), std::chrono::steady_clock::now() + maxWait_});
            timer_.notify_all();
            return;
        }
        inside_++;
    }
    admitted(true);
}

void SchemaGate::leave() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inside_ > 0) inside_--;
    if (inside_ == 0) idle_.notify_all();
}

void SchemaGate::pause(std::chrono::milliseconds maxWait) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxWait_ = maxWait;
    if (paused_) return;
    paused_ = true;
    timerThread_ = std::thread([this] { timeouts(); });
}

void SchemaGate::waitForIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return inside_ == 0; });
}

void SchemaGate::resume() {
    std::deque<Waiter> admitting;
    std::thread timer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!paused_) return;
        paused_ = false;
        admitting.swap(waiting_);
        inside_ += admitting.size();
        timer = std::move(timerThread_);
        timer_.notify_all();
    }
    if (timer.joinable()) timer.join();
    for (Waiter& w : admitting) w.admitted(true);
}

void SchemaGate::timeouts() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (paused_) {
        auto now = std::chrono::steady_clock::now();
        std::vector<Admitted> expired;
        while (!waiting_.empty() && waiting_.front().deadline <= now) {
            expired.push_back(std::move(waiting_.front().admitted));
            waiting_.pop_front();
        }
        if (!expired.empty()) {
            lock.unlock();
            for (Admitted& a : expired) a(false);
            lock.lock();
            continue;
        }
        if (waiting_.empty()) {
            timer_.wait(lock);
        } else {
            timer_.wait_until(lock, waiting_.front().deadline);
        }
    }
}

SchemaGate::Stats SchemaGate::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {inside_, waiting_.size(), paused_};
}

}  // namespace fourfold

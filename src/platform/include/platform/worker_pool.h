// WorkerPool: a fixed set of threads for work that would block the script
// thread or the I/O loop (Arch §3): file locks, database queries, outbound
// HTTP, DNS, SMTP.
//
// Work runs in the order submitted, on whichever worker is free. A task that
// needs to report back posts to the loop that should hear about it, e.g.
// `loop.post(...)`; the pool itself knows nothing about loops.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace fourfold {

class WorkerPool {
public:
    using Task = std::function<void()>;

    explicit WorkerPool(size_t threadCount);
    // Lets running tasks finish, drops any that haven't started, and joins.
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Thread-safe.
    void submit(Task task);

    size_t threadCount() const { return threads_.size(); }

private:
    void workerMain();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> queue_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace fourfold

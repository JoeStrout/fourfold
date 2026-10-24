#include "platform/worker_pool.h"

namespace fourfold {

WorkerPool::WorkerPool(size_t threadCount) {
    if (threadCount == 0) threadCount = 1;
    threads_.reserve(threadCount);
    for (size_t i = 0; i < threadCount; i++) threads_.emplace_back([this] { workerMain(); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    for (std::thread& t : threads_) t.join();
}

void WorkerPool::submit(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void WorkerPool::workerMain() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

}  // namespace fourfold

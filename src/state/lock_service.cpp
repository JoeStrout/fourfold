#include "state/lock_service.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "partition/file_lock.h"

namespace fourfold {

namespace {

// The queueing both kinds share. With a lock file, the head of a queue goes
// through Acquiring (waiting on the file, on a worker) before Held.
class QueuedLocks : public LockService {
public:
    QueuedLocks(std::function<Result<Fd>(const std::string&)> openLockFile, WorkerPool* workers)
        : openLockFile_(std::move(openLockFile)), workers_(workers) {}

    ~QueuedLocks() override {
        // A worker may still be waiting on a lock file for us; wait for it.
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return acquiring_ == 0; });
    }

    Ticket acquire(const std::string& key, Granted granted) override {
        std::vector<std::function<void()>> after;
        Ticket ticket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ticket = nextTicket_++;
            tickets_[ticket] = Entry{key, std::move(granted), State::Waiting, std::nullopt, false};
            auto& queue = queues_[key];
            queue.push_back(ticket);
            if (queue.size() == 1) startHead(key, after);
        }
        for (auto& f : after) f();
        return ticket;
    }

    void release(Ticket ticket) override {
        std::vector<std::function<void()>> after;
        std::optional<FileLock> dropped;   // released outside the mutex
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tickets_.find(ticket);
            if (it == tickets_.end()) return;
            Entry& e = it->second;
            if (e.state == State::Acquiring) {
                // A worker is waiting on the file for it; it drops the lock
                // and moves on when it gets it.
                e.cancelled = true;
                return;
            }
            std::string key = e.key;
            bool wasHead = e.state == State::Held || e.state == State::Failed;
            dropped = std::move(e.fileLock);
            tickets_.erase(it);
            auto& queue = queues_[key];
            for (auto q = queue.begin(); q != queue.end(); ++q) {
                if (*q == ticket) {
                    queue.erase(q);
                    break;
                }
            }
            if (queue.empty()) {
                queues_.erase(key);
            } else if (wasHead) {
                startHead(key, after);
            }
        }
        dropped.reset();
        for (auto& f : after) f();
    }

    Stats stats() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats s;
        for (const auto& [id, e] : tickets_) {
            if (e.state == State::Held) s.held++;
            else if (e.state != State::Failed) s.waiting++;
        }
        return s;
    }

private:
    enum class State { Waiting, Acquiring, Held, Failed };

    struct Entry {
        std::string key;
        Granted granted;
        State state;
        std::optional<FileLock> fileLock;
        bool cancelled;
    };

    // Gives the lock to the head of `key`'s queue. Callbacks to run once the
    // mutex is released go in `after`. Mutex held.
    void startHead(const std::string& key, std::vector<std::function<void()>>& after) {
        Ticket head = queues_[key].front();
        Entry& e = tickets_[head];
        if (!workers_) {
            e.state = State::Held;
            after.push_back([granted = std::move(e.granted), head] { granted(Status(), head); });
            return;
        }
        e.state = State::Acquiring;
        acquiring_++;
        workers_->submit([this, head, key] { acquireFile(head, key); });
    }

    // On a worker: waits for the file lock, then grants it (or, if the
    // ticket was released meanwhile, passes it on).
    void acquireFile(Ticket ticket, const std::string& key) {
        Status status;
        std::optional<FileLock> fileLock;
        auto fd = openLockFile_(key);
        if (!fd) {
            status = fd.error();
        } else {
            auto held = FileLock::acquire(std::move(fd).value());
            if (!held) status = held.error();
            else fileLock = std::move(held).value();
        }

        Granted granted;
        bool cancelled;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Entry& e = tickets_[ticket];
            cancelled = e.cancelled;
            if (!cancelled) {
                e.state = status ? State::Held : State::Failed;
                e.fileLock = std::move(fileLock);
                granted = std::move(e.granted);
            } else {
                e.state = State::Held;   // so release() below frees it
                e.fileLock = std::move(fileLock);
            }
        }
        if (cancelled) {
            release(ticket);
        } else {
            granted(status, ticket);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            acquiring_--;
        }
        idle_.notify_all();
    }

    std::function<Result<Fd>(const std::string&)> openLockFile_;
    WorkerPool* workers_;   // null: in memory only

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    size_t acquiring_ = 0;
    Ticket nextTicket_ = 1;
    std::unordered_map<Ticket, Entry> tickets_;
    std::unordered_map<std::string, std::deque<Ticket>> queues_;
};

}  // namespace

std::unique_ptr<LockService> makeMemoryLockService() { return std::make_unique<QueuedLocks>(nullptr, nullptr); }

std::unique_ptr<LockService> makeFileLockService(std::function<Result<Fd>(const std::string& key)> openLockFile,
                                                 WorkerPool& workers) {
    return std::make_unique<QueuedLocks>(std::move(openLockFile), &workers);
}

}  // namespace fourfold

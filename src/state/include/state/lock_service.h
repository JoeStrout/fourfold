// LockService: named exclusive locks for runs on the script thread (Arch §3,
// §5 state): a session's lock, and a data file's for data.update.
//
// Nothing here blocks the caller. acquire() returns at once; `granted` is
// called later (or right away, if the lock is free), on whichever thread
// released the lock or took it, so callers post it to their own loop. Waiters
// are served in the order they asked.
//
// Two kinds:
// - in memory, for one process (the standalone server's sessions);
// - in memory plus a lock file, for locks shared with other processes (CGI
//   mode; data files). Only the head of each key's queue waits on the file,
//   on a worker thread, so waiters can't fill the worker pool.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "base/fd.h"
#include "base/result.h"
#include "platform/worker_pool.h"

namespace fourfold {

class LockService {
public:
    using Ticket = uint64_t;
    // Called once, when the lock is held (ok) or can't be had (an error, e.g.
    // the lock file couldn't be opened), with the ticket acquire() returned
    // (which it may not have returned yet). Not called if the ticket is
    // released first.
    using Granted = std::function<void(Status, Ticket)>;

    virtual ~LockService() = default;

    // Thread-safe.
    virtual Ticket acquire(const std::string& key, Granted granted) = 0;
    // Releases a held lock, or gives up a ticket still waiting (its `granted`
    // is then never called). Every ticket must be released exactly once, even
    // after a failed grant. Thread-safe.
    virtual void release(Ticket ticket) = 0;

    struct Stats {
        size_t held = 0;      // tickets holding their lock
        size_t waiting = 0;   // tickets waiting (including on a lock file)
    };
    virtual Stats stats() const = 0;
};

std::unique_ptr<LockService> makeMemoryLockService();

// `openLockFile(key)` opens the key's lock file (e.g. DataDir::lockFile); it
// and the pool must outlive the service.
std::unique_ptr<LockService> makeFileLockService(std::function<Result<Fd>(const std::string& key)> openLockFile,
                                                 WorkerPool& workers);

}  // namespace fourfold

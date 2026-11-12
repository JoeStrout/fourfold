// FileLock: a lock on a lock file (flock), held until destroyed: exclusive,
// or shared (any number of shared holders, but no exclusive one).
// Locks between processes, e.g. a CGI process and the server (overview §8).
//
// acquire() blocks, so it's for worker threads only. The script thread never
// blocks on a lock (Arch §3): code there waits for an in-process lock first,
// and only the holder of that takes the file lock, on a worker.
#pragma once

#include <chrono>
#include <optional>

#include "base/fd.h"
#include "base/result.h"

namespace fourfold {

class FileLock {
public:
    enum class Mode { Exclusive, Shared };

    // Waits until the lock is free.
    static Result<FileLock> acquire(Fd lockFile, Mode mode = Mode::Exclusive);
    // Takes the lock if it's free; nullopt if another holder has it.
    static Result<std::optional<FileLock>> tryAcquire(Fd lockFile, Mode mode = Mode::Exclusive);
    // Waits up to `limit` for the lock (by polling); nullopt if it's still
    // held then.
    static Result<std::optional<FileLock>> acquireWithin(Fd lockFile, Mode mode, std::chrono::milliseconds limit);

    FileLock(FileLock&&) = default;
    FileLock& operator=(FileLock&&) = default;
    ~FileLock() = default;   // closing the file releases the lock

private:
    explicit FileLock(Fd fd) : fd_(std::move(fd)) {}
    Fd fd_;
};

}  // namespace fourfold

#include "partition/file_lock.h"

#include <cerrno>
#include <cstring>
#include <sys/file.h>
#include <thread>

namespace fourfold {

namespace {

int operation(FileLock::Mode mode) { return mode == FileLock::Mode::Shared ? LOCK_SH : LOCK_EX; }

}  // namespace

Result<FileLock> FileLock::acquire(Fd lockFile, Mode mode) {
    while (flock(lockFile.get(), operation(mode)) < 0) {
        if (errno != EINTR) return makeError(ErrorCode::IO, std::string("lock: ") + std::strerror(errno));
    }
    return FileLock(std::move(lockFile));
}

Result<std::optional<FileLock>> FileLock::tryAcquire(Fd lockFile, Mode mode) {
    while (flock(lockFile.get(), operation(mode) | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) return std::optional<FileLock>();
        if (errno != EINTR) return makeError(ErrorCode::IO, std::string("lock: ") + std::strerror(errno));
    }
    return std::optional<FileLock>(FileLock(std::move(lockFile)));
}

Result<std::optional<FileLock>> FileLock::acquireWithin(Fd lockFile, Mode mode, std::chrono::milliseconds limit) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    auto pause = std::chrono::milliseconds(5);
    while (flock(lockFile.get(), operation(mode) | LOCK_NB) < 0) {
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK) return makeError(ErrorCode::IO, std::string("lock: ") + std::strerror(errno));
        if (std::chrono::steady_clock::now() >= deadline) return std::optional<FileLock>();
        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, std::chrono::milliseconds(100));
    }
    return std::optional<FileLock>(FileLock(std::move(lockFile)));
}

}  // namespace fourfold

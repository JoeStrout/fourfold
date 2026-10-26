#include "partition/file_lock.h"

#include <cerrno>
#include <cstring>
#include <sys/file.h>

namespace fourfold {

Result<FileLock> FileLock::acquire(Fd lockFile) {
    while (flock(lockFile.get(), LOCK_EX) < 0) {
        if (errno != EINTR) return makeError(ErrorCode::IO, std::string("lock: ") + std::strerror(errno));
    }
    return FileLock(std::move(lockFile));
}

Result<std::optional<FileLock>> FileLock::tryAcquire(Fd lockFile) {
    while (flock(lockFile.get(), LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) return std::optional<FileLock>();
        if (errno != EINTR) return makeError(ErrorCode::IO, std::string("lock: ") + std::strerror(errno));
    }
    return std::optional<FileLock>(FileLock(std::move(lockFile)));
}

}  // namespace fourfold

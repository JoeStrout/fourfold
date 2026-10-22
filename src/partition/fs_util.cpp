#include "fs_util.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace fourfold {

Error fileError(int err, const std::string& what) {
    switch (err) {
        case ENOENT:
        case ENOTDIR:
            return makeError(ErrorCode::NotFound, what + ": not found");
        case ELOOP:
#ifdef EMLINK
        case EMLINK:   // FreeBSD's O_NOFOLLOW error
#endif
            return makeError(ErrorCode::NotFound, what + ": symbolic link (not followed)");
        case EACCES:
        case EPERM:
            return makeError(ErrorCode::Denied, what + ": permission denied");
        default:
            return makeError(ErrorCode::IO, what + ": " + std::strerror(err));
    }
}

std::string describe(const std::string& partition, const RelPath& path) {
    return path.isRoot() ? partition : partition + "/" + path.str();
}

Result<Fd> openFolder(int rootFd, const RelPath& dir, const std::string& partition) {
    // "." gives a fresh open file description, with its own read position.
    Fd current(openat(rootFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current.valid()) return fileError(errno, partition);
    for (std::string_view segment : dir.segments()) {
        std::string name(segment);
        Fd next(openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next.valid()) return fileError(errno, describe(partition, dir));
        current = std::move(next);
    }
    return current;
}

Result<Fd> openFileForRead(int rootFd, const RelPath& path, const std::string& partition,
                           struct stat* info) {
    if (path.isRoot()) return makeError(ErrorCode::NotFound, partition + ": not a file");
    auto folder = openFolder(rootFd, path.parent(), partition);
    if (!folder) return folder.error();
    std::string name(path.name());
    // O_NONBLOCK: opening a FIFO mustn't hang. (It's then rejected below.)
    Fd fd(openat(folder.value().get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd.valid()) return fileError(errno, describe(partition, path));
    if (fstat(fd.get(), info) < 0) return fileError(errno, describe(partition, path));
    if (!S_ISREG(info->st_mode)) {
        return makeError(ErrorCode::NotFound, describe(partition, path) + ": not a regular file");
    }
    return fd;
}

int64_t modifiedTimeNs(const struct stat& info) {
#ifdef __APPLE__
    const timespec& t = info.st_mtimespec;
#else
    const timespec& t = info.st_mtim;
#endif
    return static_cast<int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}

}  // namespace fourfold

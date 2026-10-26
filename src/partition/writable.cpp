#include "writable.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>

#include "fs_util.h"

namespace fourfold::writable {

namespace {

Result<Fd> openOrMakeFolder(int parentFd, const std::string& name, mode_t mode, const std::string& what) {
    Fd fd(openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.valid()) return fd;
    if (errno != ENOENT) return fileError(errno, what);
    if (mkdirat(parentFd, name.c_str(), mode) < 0 && errno != EEXIST) return fileError(errno, what);
    fd = Fd(openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd.valid()) return fileError(errno, what);
    return fd;
}

Result<Fd> rootCopy(int rootFd, const std::string& partition) {
    Fd fd(openat(rootFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!fd.valid()) return fileError(errno, partition);
    return fd;
}

// A name for a temporary file, unique among this process's and (by the pid)
// other processes'. O_EXCL makes a clash an error rather than a clobber.
std::string tempName() {
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + "-" +
           std::to_string(static_cast<uint64_t>(now) & 0xFFFFFF);
}

Status writeAll(int fd, std::string_view bytes, const std::string& what) {
    while (!bytes.empty()) {
        ssize_t n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return fileError(errno, what);
        }
        bytes.remove_prefix(static_cast<size_t>(n));
    }
    return {};
}

// Opens `name` in `dirFd`, creating it if missing (O_CREAT, never following
// a link). macOS can fail such an open with ENOENT while another process or
// thread creates the same file; trying again finds it there. errno is set
// if the result isn't valid.
Fd openCreating(int dirFd, const std::string& name, int flags, mode_t mode) {
    Fd fd;
    for (int attempt = 0; attempt < 5 && !fd.valid(); attempt++) {
        fd = Fd(openat(dirFd, name.c_str(), flags | O_CREAT | O_NOFOLLOW | O_CLOEXEC, mode));
        if (!fd.valid() && errno != ENOENT) break;
    }
    return fd;
}

// The file's folder (created if missing), or an error for the root.
Result<Fd> folderFor(int rootFd, const RelPath& path, mode_t fileMode, const std::string& partition) {
    if (path.isRoot()) return makeError(ErrorCode::Invalid, partition + ": not a file");
    // Folders get search permission wherever the file mode has read.
    mode_t folderMode = fileMode | ((fileMode & 0444) >> 2);
    return ensureFolder(rootFd, path.parent(), folderMode, partition);
}

}  // namespace

Result<Fd> ensureFolder(int rootFd, const RelPath& dir, mode_t mode, const std::string& partition) {
    auto current = rootCopy(rootFd, partition);
    if (!current) return current.error();
    Fd fd = std::move(current).value();
    for (std::string_view segment : dir.segments()) {
        auto next = openOrMakeFolder(fd.get(), std::string(segment), mode, describe(partition, dir));
        if (!next) return next.error();
        fd = std::move(next).value();
    }
    return fd;
}

Result<Fd> ensureEngineFolder(int rootFd, const std::vector<std::string>& segments, mode_t mode,
                              const std::string& partition) {
    auto current = rootCopy(rootFd, partition);
    if (!current) return current.error();
    Fd fd = std::move(current).value();
    std::string what = partition;
    for (const std::string& segment : segments) {
        what += "/" + segment;
        auto next = openOrMakeFolder(fd.get(), segment, mode, what);
        if (!next) return next.error();
        fd = std::move(next).value();
    }
    return fd;
}

Result<std::string> readFile(int rootFd, const RelPath& path, size_t maxBytes, const std::string& partition) {
    struct stat info;
    auto fd = openFileForRead(rootFd, path, partition, &info);
    if (!fd) return fd.error();
    std::string what = describe(partition, path);
    if (static_cast<uint64_t>(info.st_size) > maxBytes) {
        return makeError(ErrorCode::TooLarge, what + ": over " + std::to_string(maxBytes) + " bytes");
    }
    std::string out;
    out.resize(static_cast<size_t>(info.st_size));
    size_t got = 0;
    while (true) {
        if (got == out.size()) {
            if (out.size() >= maxBytes) {
                // It may have grown since the stat; one more byte says so.
                char extra;
                ssize_t n = ::read(fd.value().get(), &extra, 1);
                if (n > 0) {
                    return makeError(ErrorCode::TooLarge, what + ": over " + std::to_string(maxBytes) + " bytes");
                }
                break;
            }
            out.resize(std::min(maxBytes, out.size() * 2 + 4096));
        }
        ssize_t n = ::read(fd.value().get(), out.data() + got, out.size() - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return fileError(errno, what);
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    out.resize(got);
    return out;
}

namespace {

// Writes `bytes` to a new temporary file in `dirFd` and flushes it; returns
// its name.
Result<std::string> writeTemp(int dirFd, std::string_view bytes, mode_t mode, const std::string& what) {
    std::string temp = tempName();
    Fd fd(openat(dirFd, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (!fd.valid()) return fileError(errno, what);
    Status s = writeAll(fd.get(), bytes, what);
    if (s && ::fsync(fd.get()) < 0) s = fileError(errno, what);
    if (!s) {
        unlinkat(dirFd, temp.c_str(), 0);
        return s.error();
    }
    return temp;
}

}  // namespace

Status writeAtomic(int rootFd, const RelPath& path, std::string_view bytes, mode_t mode,
                   const std::string& partition) {
    auto folder = folderFor(rootFd, path, mode, partition);
    if (!folder) return folder.error();
    int dirFd = folder.value().get();
    std::string what = describe(partition, path);
    std::string name(path.name());

    // The temporary file has a dot-name, so listings and RelPath never see it.
    auto temp = writeTemp(dirFd, bytes, mode, what);
    if (!temp) return temp.error();
    Status s;
    // Don't replace a folder or anything else that isn't a plain file.
    struct stat info;
    if (fstatat(dirFd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(info.st_mode)) {
        s = makeError(ErrorCode::Conflict, what + ": exists and isn't a file");
    }
    if (s && renameat(dirFd, temp.value().c_str(), dirFd, name.c_str()) < 0) s = fileError(errno, what);
    if (!s) {
        unlinkat(dirFd, temp.value().c_str(), 0);
        return s;
    }
    ::fsync(dirFd);   // the rename itself, durable
    return {};
}

Status createAtomic(int rootFd, const RelPath& path, std::string_view bytes, mode_t mode,
                    const std::string& partition) {
    auto folder = folderFor(rootFd, path, mode, partition);
    if (!folder) return folder.error();
    int dirFd = folder.value().get();
    std::string what = describe(partition, path);
    std::string name(path.name());
    auto temp = writeTemp(dirFd, bytes, mode, what);
    if (!temp) return temp.error();
    // link() fails if the name exists, unlike rename().
    int rc = linkat(dirFd, temp.value().c_str(), dirFd, name.c_str(), 0);
    int err = errno;
    unlinkat(dirFd, temp.value().c_str(), 0);
    if (rc < 0) {
        if (err == EEXIST) return makeError(ErrorCode::Conflict, what + ": already exists");
        return fileError(err, what);
    }
    ::fsync(dirFd);
    return {};
}

Status append(int rootFd, const RelPath& path, std::string_view bytes, mode_t mode, const std::string& partition) {
    auto folder = folderFor(rootFd, path, mode, partition);
    if (!folder) return folder.error();
    std::string what = describe(partition, path);
    std::string name(path.name());
    Fd fd = openCreating(folder.value().get(), name, O_WRONLY | O_APPEND | O_NONBLOCK, mode);
    if (!fd.valid()) return fileError(errno, what);
    struct stat info;
    if (fstat(fd.get(), &info) < 0) return fileError(errno, what);
    if (!S_ISREG(info.st_mode)) return makeError(ErrorCode::NotFound, what + ": not a regular file");
    // One write call, so concurrent appends can't interleave. A short write
    // (a full disk) is finished with more writes, and reported if it fails.
    return writeAll(fd.get(), bytes, what);
}

Status removeFile(int rootFd, const RelPath& path, const std::string& partition) {
    if (path.isRoot()) return makeError(ErrorCode::Invalid, partition + ": not a file");
    auto folder = openFolder(rootFd, path.parent(), partition);
    if (!folder) return folder.error();
    std::string name(path.name());
    struct stat info;
    if (fstatat(folder.value().get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) {
        return fileError(errno, describe(partition, path));
    }
    if (!S_ISREG(info.st_mode)) return makeError(ErrorCode::NotFound, describe(partition, path) + ": not a file");
    if (unlinkat(folder.value().get(), name.c_str(), 0) < 0) return fileError(errno, describe(partition, path));
    return {};
}

Result<bool> fileExists(int rootFd, const RelPath& path, const std::string& partition) {
    if (path.isRoot()) return false;
    auto folder = openFolder(rootFd, path.parent(), partition);
    if (!folder) {
        if (folder.error().code == ErrorCode::NotFound) return false;
        return folder.error();
    }
    std::string name(path.name());
    struct stat info;
    if (fstatat(folder.value().get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT || errno == ENOTDIR) return false;
        return fileError(errno, describe(partition, path));
    }
    return S_ISREG(info.st_mode);
}

Status makeFolder(int rootFd, const RelPath& dir, mode_t mode, const std::string& partition) {
    // openOrMakeFolder refuses (as ENOTDIR or ELOOP) a file or link in the way.
    auto fd = ensureFolder(rootFd, dir, mode, partition);
    if (!fd) {
        Error e = fd.error();
        if (e.code == ErrorCode::NotFound) {
            return makeError(ErrorCode::Conflict, describe(partition, dir) + ": a file is in the way");
        }
        return e;
    }
    return {};
}

Status removeFolder(int rootFd, const RelPath& dir, const std::string& partition) {
    if (dir.isRoot()) return makeError(ErrorCode::Invalid, partition + ": can't remove the partition itself");
    auto folder = openFolder(rootFd, dir.parent(), partition);
    if (!folder) return folder.error();
    std::string what = describe(partition, dir);
    std::string name(dir.name());
    struct stat st;
    if (fstatat(folder.value().get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) return fileError(errno, what);
    if (!S_ISDIR(st.st_mode)) return makeError(ErrorCode::NotFound, what + ": not a folder");
    if (unlinkat(folder.value().get(), name.c_str(), AT_REMOVEDIR) < 0) {
        if (errno == ENOTEMPTY || errno == EEXIST) return makeError(ErrorCode::Conflict, what + ": not empty");
        return fileError(errno, what);
    }
    return {};
}

Result<std::optional<FileInfo>> info(int rootFd, const RelPath& path, const std::string& partition) {
    struct stat st;
    if (path.isRoot()) {
        if (fstat(rootFd, &st) < 0) return fileError(errno, partition);
    } else {
        auto folder = openFolder(rootFd, path.parent(), partition);
        if (!folder) {
            if (folder.error().code == ErrorCode::NotFound) return std::optional<FileInfo>();
            return folder.error();
        }
        std::string name(path.name());
        if (fstatat(folder.value().get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT || errno == ENOTDIR) return std::optional<FileInfo>();
            return fileError(errno, describe(partition, path));
        }
    }
    FileInfo out;
    if (S_ISDIR(st.st_mode)) out.isFolder = true;
    else if (S_ISREG(st.st_mode)) out.size = static_cast<uint64_t>(st.st_size);
    else return std::optional<FileInfo>();
    out.modifiedNs = modifiedTimeNs(st);
    return std::optional<FileInfo>(out);
}

Result<std::vector<DirEntry>> list(int rootFd, const RelPath& dir, const std::string& partition) {
    auto folder = openFolder(rootFd, dir, partition);
    if (!folder) {
        if (folder.error().code == ErrorCode::NotFound) return std::vector<DirEntry>{};
        return folder.error();
    }
    int fd = folder.value().release();   // fdopendir takes it over
    DIR* d = fdopendir(fd);
    if (!d) {
        int err = errno;
        ::close(fd);
        return fileError(err, describe(partition, dir));
    }
    std::vector<DirEntry> out;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        if (!RelPath::parse(name)) continue;   // names RelPath couldn't reach anyway
        struct stat info;
        if (fstatat(dirfd(d), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) continue;
        if (S_ISREG(info.st_mode)) out.push_back({name, false});
        else if (S_ISDIR(info.st_mode)) out.push_back({name, true});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return out;
}

Result<Fd> openLockFile(int folderFd, const std::string& name, const std::string& what) {
    Fd fd = openCreating(folderFd, name, O_RDWR, 0600);
    if (!fd.valid()) return fileError(errno, what);
    return fd;
}

}  // namespace fourfold::writable

// Copying a partition's whole tree out to a backup folder, and back
// (overview §3, Backups). Engine use only: dot-names (data/.fourfold/) are
// included, and the caller says what to leave out. Inside the partition,
// files are opened one segment at a time without following links, as
// everywhere else; links and special files are never copied.

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs_util.h"
#include "writable.h"

namespace fourfold::writable {

namespace {

Error osError(const std::string& what) { return makeError(ErrorCode::IO, what + ": " + std::strerror(errno)); }

struct Entry {
    std::string name;
    bool folder = false;
};

// The regular files and folders in an open folder (no links), with dot-names.
Result<std::vector<Entry>> entries(int folderFd, const std::string& what) {
    int copy = fcntl(folderFd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) return osError(what);
    DIR* dir = fdopendir(copy);
    if (!dir) {
        ::close(copy);
        return osError(what);
    }
    rewinddir(dir);
    std::vector<Entry> out;
    while (struct dirent* e = readdir(dir)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat info;
        if (fstatat(folderFd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) continue;
        if (S_ISREG(info.st_mode)) out.push_back({name, false});
        if (S_ISDIR(info.st_mode)) out.push_back({name, true});
    }
    closedir(dir);
    return out;
}

Status writeAll(int fd, const char* data, size_t size, const std::string& what) {
    while (size > 0) {
        ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return osError(what);
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return {};
}

Result<Fd> openFolderAt(int parent, const std::string& name, bool create, const std::string& what) {
    if (create && mkdirat(parent, name.c_str(), 0700) < 0 && errno != EEXIST) return osError(what);
    Fd fd(openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd.valid()) return osError(what);
    return fd;
}

Status copyBetween(int fromFd, int toFd, const std::string& rel, const TreeSkip& skip, const std::string& what) {
    auto list = entries(fromFd, what + "/" + rel);
    if (!list) return list.error();
    for (const Entry& e : list.value()) {
        std::string path = rel.empty() ? e.name : rel + "/" + e.name;
        if (skip && skip(path, e.folder)) continue;
        if (e.folder) {
            auto from = openFolderAt(fromFd, e.name, false, what + "/" + path);
            if (!from) return from.error();
            auto to = openFolderAt(toFd, e.name, true, what + "/" + path);
            if (!to) return to.error();
            if (Status s = copyBetween(from.value().get(), to.value().get(), path, skip, what); !s) return s;
            continue;
        }
        Fd in(openat(fromFd, e.name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (!in.valid()) return osError(what + "/" + path);
        struct stat info;
        mode_t mode = fstat(in.get(), &info) == 0 ? (info.st_mode & 0777) : 0600;   // as it was
        if (Status s = copyFileInto(in.get(), toFd, e.name, mode, what + "/" + path); !s) return s;
    }
    return {};
}

// Removes everything in folder `fd` that `skip` doesn't keep (a folder is
// removed only once it's empty).
Status clear(int fd, const std::string& rel, const TreeSkip& skip, const std::string& what) {
    auto list = entries(fd, what + "/" + rel);
    if (!list) return list.error();
    for (const Entry& e : list.value()) {
        std::string path = rel.empty() ? e.name : rel + "/" + e.name;
        if (skip && skip(path, e.folder)) continue;
        if (e.folder) {
            auto sub = openFolderAt(fd, e.name, false, what + "/" + path);
            if (!sub) return sub.error();
            if (Status s = clear(sub.value().get(), path, skip, what); !s) return s;
            if (unlinkat(fd, e.name.c_str(), AT_REMOVEDIR) < 0 && errno != ENOTEMPTY && errno != EEXIST) {
                return osError(what + "/" + path);
            }
            continue;
        }
        if (unlinkat(fd, e.name.c_str(), 0) < 0 && errno != ENOENT) return osError(what + "/" + path);
    }
    return {};
}

}  // namespace

Status copyFileInto(int from, int toFd, const std::string& name, mode_t mode, const std::string& what,
                    std::optional<int64_t> modifiedNs) {
    Fd out(openat(toFd, name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode));
    if (!out.valid()) return osError(what);
    char buffer[64 * 1024];
    while (true) {
        ssize_t n = ::read(from, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) continue;
            return osError(what);
        }
        if (n == 0) break;
        if (Status s = writeAll(out.get(), buffer, static_cast<size_t>(n), what); !s) return s;
    }
    if (modifiedNs) {
        struct timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;   // the access time
        times[1].tv_sec = static_cast<time_t>(*modifiedNs / 1'000'000'000);
        times[1].tv_nsec = static_cast<long>(*modifiedNs % 1'000'000'000);
        if (futimens(out.get(), times) < 0) return osError(what);
    }
    if (fsync(out.get()) < 0) return osError(what);
    return {};
}

Status removeTree(int rootFd, const RelPath& dir, const std::string& partition) {
    if (dir.isRoot()) return makeError(ErrorCode::Invalid, partition + ": can't remove the partition itself");
    auto parent = openFolder(rootFd, dir.parent(), partition);
    if (!parent) return parent.error().code == ErrorCode::NotFound ? Status() : Status(parent.error());
    std::string name(dir.name());
    std::string what = describe(partition, dir);
    struct stat st;
    if (fstatat(parent.value().get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) {
        return errno == ENOENT ? Status() : Status(osError(what));
    }
    if (!S_ISDIR(st.st_mode)) return makeError(ErrorCode::Conflict, what + ": not a folder");
    auto folder = openFolderAt(parent.value().get(), name, false, what);
    if (!folder) return folder.error();
    if (Status s = clear(folder.value().get(), "", nullptr, what); !s) return s;
    if (unlinkat(parent.value().get(), name.c_str(), AT_REMOVEDIR) < 0 && errno != ENOENT) return osError(what);
    return {};
}

Status copyTreeOut(int fromFd, const std::string& toHostFolder, const TreeSkip& skip, const std::string& partition) {
    if (mkdir(toHostFolder.c_str(), 0700) < 0 && errno != EEXIST) return osError(toHostFolder);
    Fd to(::open(toHostFolder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!to.valid()) return osError(toHostFolder);
    return copyBetween(fromFd, to.get(), "", skip, partition);
}

Status clearTree(int fd, const TreeSkip& skip, const std::string& partition) { return clear(fd, "", skip, partition); }

Status copyTreeIn(const std::string& fromHostFolder, int toFd, const TreeSkip& skip, const std::string& partition) {
    Fd from(::open(fromHostFolder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!from.valid()) return osError(fromHostFolder);
    if (Status s = clear(toFd, "", skip, partition); !s) return s;
    return copyBetween(from.get(), toFd, "", skip, partition);
}

}  // namespace fourfold::writable

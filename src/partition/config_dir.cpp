#include "partition/config_dir.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs_util.h"
#include "partition/paths.h"

namespace fourfold {

namespace {

const char* const kPartition = "private/config";

// The rest of an open regular file of `size` bytes, up to kMaxBytes.
Result<std::string> readAll(int fd, const struct stat& info, const std::string& what) {
    if (static_cast<uint64_t>(info.st_size) > ConfigDir::kMaxBytes) {
        return makeError(ErrorCode::TooLarge, what + ": file too large (the limit is 1 MB)");
    }
    std::string contents;
    char buffer[8192];
    for (;;) {
        ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fileError(errno, what);
        }
        if (n == 0) break;
        contents.append(buffer, static_cast<size_t>(n));
        if (contents.size() > ConfigDir::kMaxBytes) {
            return makeError(ErrorCode::TooLarge, what + ": file too large (the limit is 1 MB)");
        }
    }
    return contents;
}

}  // namespace

Result<std::optional<ConfigFile>> ConfigDir::read(std::string_view name) const {
    auto path = RelPath::parse(name);
    if (!path) return path.error();
    if (path.value().segments().size() != 1) return makeError(ErrorCode::Invalid, "config files have no folders");
    std::string what = describe(kPartition, path.value());
    struct stat info;
    if (fstatat(fd_.get(), path.value().str().c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) return std::optional<ConfigFile>();
        return fileError(errno, what);
    }
    auto fd = openFileForRead(fd_.get(), path.value(), kPartition, &info);
    if (!fd) return fd.error();
    auto contents = readAll(fd.value().get(), info, what);
    if (!contents) return contents.error();
    return std::optional<ConfigFile>(ConfigFile(what, std::move(contents).value()));
}

std::string ConfigDir::stamp(std::string_view name) const {
    std::string n(name);
    struct stat info;
    if (fstatat(fd_.get(), n.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) return "";
    return std::to_string(info.st_size) + ":" + std::to_string(modifiedTimeNs(info)) + ":" +
           std::to_string(info.st_mode);
}

Result<ConfigFile> ConfigDir::readServerConfig(const std::string& hostPath) {
    Fd fd(::open(hostPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!fd.valid()) return fileError(errno, hostPath);
    struct stat info;
    if (fstat(fd.get(), &info) < 0) return fileError(errno, hostPath);
    if (!S_ISREG(info.st_mode)) return makeError(ErrorCode::NotFound, hostPath + ": not a regular file");
    auto contents = readAll(fd.get(), info, hostPath);
    if (!contents) return contents.error();
    return ConfigFile(hostPath, std::move(contents).value());
}

}  // namespace fourfold

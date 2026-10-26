#include "partition/data_dir.h"

#include "base/crypto.h"
#include "writable.h"

namespace fourfold {

namespace {

const char* const kData = "data";
const char* const kDropbox = "dropbox";
const char* const kState = "private/state";

// A lock file's name: the key hashed, so any key makes a safe, fixed-length
// name.
std::string lockName(std::string_view key) { return toHex(hashBytes(key)).substr(0, 32) + ".lock"; }

Result<Fd> lockFileIn(int rootFd, const std::vector<std::string>& folder, std::string_view key,
                      const std::string& partition) {
    auto dir = writable::ensureEngineFolder(rootFd, folder, 0700, partition);
    if (!dir) return dir.error();
    return writable::openLockFile(dir.value().get(), lockName(key), partition + " lock");
}

}  // namespace

Result<std::string> DataDir::read(const RelPath& path, size_t maxBytes) const {
    return writable::readFile(fd_.get(), path, maxBytes, kData);
}

Status DataDir::write(const RelPath& path, std::string_view bytes) const {
    return writable::writeAtomic(fd_.get(), path, bytes, 0644, kData);
}

Status DataDir::append(const RelPath& path, std::string_view bytes) const {
    return writable::append(fd_.get(), path, bytes, 0644, kData);
}

Status DataDir::remove(const RelPath& path) const { return writable::removeFile(fd_.get(), path, kData); }

Result<bool> DataDir::exists(const RelPath& path) const { return writable::fileExists(fd_.get(), path, kData); }

Result<std::vector<DirEntry>> DataDir::list(const RelPath& folder) const {
    return writable::list(fd_.get(), folder, kData);
}

Result<Fd> DataDir::lockFile(std::string_view key) const {
    return lockFileIn(fd_.get(), {".fourfold", "locks"}, key, kData);
}

Status Dropbox::append(const RelPath& path, std::string_view bytes) const {
    return writable::append(fd_.get(), path, bytes, 0644, kDropbox);
}

Result<std::string> StateDir::read(const RelPath& path, size_t maxBytes) const {
    return writable::readFile(fd_.get(), path, maxBytes, kState);
}

Status StateDir::write(const RelPath& path, std::string_view bytes) const {
    return writable::writeAtomic(fd_.get(), path, bytes, 0600, kState);
}

Status StateDir::create(const RelPath& path, std::string_view bytes) const {
    return writable::createAtomic(fd_.get(), path, bytes, 0600, kState);
}

Status StateDir::append(const RelPath& path, std::string_view bytes) const {
    return writable::append(fd_.get(), path, bytes, 0600, kState);
}

Result<bool> StateDir::exists(const RelPath& path) const { return writable::fileExists(fd_.get(), path, kState); }

Result<Fd> StateDir::lockFile(std::string_view key) const { return lockFileIn(fd_.get(), {"locks"}, key, kState); }

Result<std::string> StateDir::sqlitePath(const RelPath& path) const {
    if (path.isRoot()) return makeError(ErrorCode::Invalid, std::string(kState) + ": not a file");
    auto folder = writable::ensureFolder(fd_.get(), path.parent(), 0700, kState);
    if (!folder) return folder.error();
    return hostPath_ + "/" + path.str();
}

}  // namespace fourfold

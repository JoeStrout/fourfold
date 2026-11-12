#include "partition/data_dir.h"

#include "base/crypto.h"
#include <sys/stat.h>

#include "fs_util.h"
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

Status DataDir::makeFolder(const RelPath& folder) const {
    return writable::makeFolder(fd_.get(), folder, 0755, kData);
}

Status DataDir::removeFolder(const RelPath& folder) const {
    return writable::removeFolder(fd_.get(), folder, kData);
}

Result<std::optional<FileInfo>> DataDir::info(const RelPath& path) const {
    return writable::info(fd_.get(), path, kData);
}

Result<Fd> DataDir::lockFile(std::string_view key) const {
    return lockFileIn(fd_.get(), {".fourfold", "locks"}, key, kData);
}

Result<std::optional<std::string>> DataDir::readEngineFile(const std::string& name, size_t maxBytes) const {
    auto dir = writable::ensureEngineFolder(fd_.get(), {".fourfold"}, 0700, kData);
    if (!dir) return dir.error();
    return writable::readEngineFile(dir.value().get(), name, maxBytes, std::string(kData) + "/.fourfold/" + name);
}

Status DataDir::writeEngineFile(const std::string& name, std::string_view bytes) const {
    auto dir = writable::ensureEngineFolder(fd_.get(), {".fourfold"}, 0700, kData);
    if (!dir) return dir.error();
    return writable::writeEngineFile(dir.value().get(), name, bytes, 0600, std::string(kData) + "/.fourfold/" + name);
}

Result<std::string> DataDir::databasePath() const {
    if (hostPath_.empty()) return makeError(ErrorCode::Invalid, "data/: no host path for the database");
    auto dir = writable::ensureEngineFolder(fd_.get(), {".fourfold"}, 0700, kData);
    if (!dir) return dir.error();
    return hostPath_ + "/.fourfold/app.sqlite";
}

Status DataDir::copyOut(const std::string& hostFolder, const TreeSkip& skip) const {
    return writable::copyTreeOut(fd_.get(), hostFolder, skip, kData);
}

Status DataDir::copyIn(const std::string& hostFolder, const TreeSkip& skip) const {
    return writable::copyTreeIn(hostFolder, fd_.get(), skip, kData);
}

Status DataDir::clear(const TreeSkip& skip) const { return writable::clearTree(fd_.get(), skip, kData); }

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

Status StateDir::remove(const RelPath& path) const { return writable::removeFile(fd_.get(), path, kState); }

Result<Fd> StateDir::lockFile(std::string_view key) const { return lockFileIn(fd_.get(), {"locks"}, key, kState); }

Result<std::string> StateDir::sqlitePath(const RelPath& path) const {
    if (path.isRoot()) return makeError(ErrorCode::Invalid, std::string(kState) + ": not a file");
    auto folder = writable::ensureFolder(fd_.get(), path.parent(), 0700, kState);
    if (!folder) return folder.error();
    return hostPath_ + "/" + path.str();
}

Result<std::string> StateDir::folderPath(const RelPath& name) const {
    if (name.isRoot()) return makeError(ErrorCode::Invalid, std::string(kState) + ": not a folder name");
    auto folder = writable::ensureFolder(fd_.get(), name, 0700, kState);
    if (!folder) return folder.error();
    return hostPath_ + "/" + name.str();
}

Status StateDir::copyOut(const std::string& hostFolder, const TreeSkip& skip) const {
    return writable::copyTreeOut(fd_.get(), hostFolder, skip, kState);
}

Result<std::vector<DirEntry>> StateDir::list(const RelPath& folder) const {
    return writable::list(fd_.get(), folder, kState);
}

Status StateDir::removeTree(const RelPath& folder) const { return writable::removeTree(fd_.get(), folder, kState); }

Status StateDir::saveCodeSnapshot(const CodeTree& code, const CodeListing& listing, const RelPath& to) const {
    if (to.isRoot()) return makeError(ErrorCode::Invalid, std::string(kState) + ": not a snapshot folder");
    if (Status s = removeTree(to); !s) return s;
    auto root = writable::ensureFolder(fd_.get(), to, 0700, kState);
    if (!root) return root.error();
    for (const CodeEntry& entry : listing.files) {
        struct stat info;
        auto in = openFileForRead(code.root_.get(), entry.path, "code", &info);
        if (!in) {
            if (in.error().code == ErrorCode::NotFound) continue;   // gone since the listing
            return in.error();
        }
        auto folder = writable::ensureFolder(root.value().get(), entry.path.parent(), 0700, kState);
        if (!folder) return folder.error();
        Status s = writable::copyFileInto(in.value().get(), folder.value().get(), std::string(entry.path.name()), 0600,
                                          describe(kState, to) + "/" + entry.path.str(), modifiedTimeNs(info));
        if (!s) return s;
    }
    return {};
}

Result<CodeTree> StateDir::openCodeSnapshot(const RelPath& folder, CodeTreeLimits limits) const {
    if (folder.isRoot()) return makeError(ErrorCode::Invalid, std::string(kState) + ": not a snapshot folder");
    auto fd = openFolder(fd_.get(), folder, kState);
    if (!fd) return fd.error();
    return CodeTree(std::move(fd).value(), limits);
}

Status StateDir::copyIn(const std::string& hostFolder, const TreeSkip& skip) const {
    return writable::copyTreeIn(hostFolder, fd_.get(), skip, kState);
}

}  // namespace fourfold

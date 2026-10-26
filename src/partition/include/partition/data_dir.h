// The writable partitions (overview §1; Arch §5 partition):
//
//   DataDir   data/            the app's data: read, atomic write, append, list, delete
//   Dropbox   dropbox/         the app's append-only output
//   StateDir  private/state/   the engine's own files; never reachable by app code
//
// All take RelPath, so ".." and dot-names can't be named, and open files one
// segment at a time without following links (see fs_util.h). Errors name
// partition-relative paths only. Each type's methods may be called from any
// thread; they block on disk I/O, so the script thread calls them through
// the worker pool.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/fd.h"
#include "base/result.h"
#include "partition/file_lock.h"
#include "partition/paths.h"

namespace fourfold {

struct DirEntry {
    std::string name;
    bool isFolder = false;
};

struct FileInfo {
    bool isFolder = false;
    uint64_t size = 0;          // bytes; 0 for a folder
    int64_t modifiedNs = 0;     // since the epoch
};

class DataDir {
public:
    // Over `fd`, an open data/ folder.
    explicit DataDir(Fd fd) : fd_(std::move(fd)) {}

    // TooLarge if the file is over `maxBytes`.
    Result<std::string> read(const RelPath& path, size_t maxBytes) const;
    // Readers see the old contents or the new, never a mix (atomic rename).
    // Creates missing folders.
    Status write(const RelPath& path, std::string_view bytes) const;
    // One write call; creates the file and folders if needed.
    Status append(const RelPath& path, std::string_view bytes) const;
    Status remove(const RelPath& path) const;
    Result<bool> exists(const RelPath& path) const;
    // Sorted; leaves out dot-names, links and special files.
    Result<std::vector<DirEntry>> list(const RelPath& folder) const;
    // Creates a folder (and any above it); fine if it exists.
    Status makeFolder(const RelPath& folder) const;
    // An empty folder only: Conflict if it has anything in it.
    Status removeFolder(const RelPath& folder) const;
    // nullopt if there's no file or folder there.
    Result<std::optional<FileInfo>> info(const RelPath& path) const;

    // The lock file for `key`, in data/.fourfold/locks/ (engine use only:
    // RelPath can't name that folder, so app code can't reach it).
    Result<Fd> lockFile(std::string_view key) const;

private:
    Fd fd_;
};

class Dropbox {
public:
    explicit Dropbox(Fd fd) : fd_(std::move(fd)) {}

    // The only operation: the app can't read back, list, or remove.
    Status append(const RelPath& path, std::string_view bytes) const;

private:
    Fd fd_;
};

class StateDir {
public:
    // `hostPath` is the folder's real path, for SQLite, which opens its
    // database by name.
    StateDir(Fd fd, std::string hostPath) : fd_(std::move(fd)), hostPath_(std::move(hostPath)) {}

    Result<std::string> read(const RelPath& path, size_t maxBytes) const;
    // Files are created readable by the engine's user only (0600).
    Status write(const RelPath& path, std::string_view bytes) const;
    // Writes `path` only if it doesn't exist (Conflict if it does): of
    // several processes racing to make it, one wins.
    Status create(const RelPath& path, std::string_view bytes) const;
    Status append(const RelPath& path, std::string_view bytes) const;
    Result<bool> exists(const RelPath& path) const;
    // The lock file for `key`, in private/state/locks/.
    Result<Fd> lockFile(std::string_view key) const;

    // The host path of a file here ("…/private/state/state.db"), for SQLite
    // only. The file's folder is created if missing.
    Result<std::string> sqlitePath(const RelPath& path) const;

private:
    Fd fd_;
    std::string hostPath_;
};

}  // namespace fourfold

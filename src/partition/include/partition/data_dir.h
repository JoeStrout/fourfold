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
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/fd.h"
#include "base/result.h"
#include "partition/code_tree.h"
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

// What a backup's copy of a partition leaves out: given a path relative to
// the partition ("a/b.txt", ".fourfold/locks") and whether it's a folder,
// true to skip it (and, for a folder, everything in it).
using TreeSkip = std::function<bool(const std::string& path, bool folder)>;

class DataDir {
public:
    // Over `fd`, an open data/ folder; `hostPath` is its real path, for
    // SQLite, which opens its database by name (empty: no database).
    explicit DataDir(Fd fd, std::string hostPath = {}) : fd_(std::move(fd)), hostPath_(std::move(hostPath)) {}

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

    // --- Engine use only: data/.fourfold/, which app code can't name. ---

    // A file in data/.fourfold/ ("migrations.json"): nullopt if missing.
    Result<std::optional<std::string>> readEngineFile(const std::string& name, size_t maxBytes) const;
    // Replaced all at once.
    Status writeEngineFile(const std::string& name, std::string_view bytes) const;
    // The host path of the app's SQLite database, data/.fourfold/app.sqlite
    // (its folder is created if missing). Fails if there's no host path.
    Result<std::string> databasePath() const;

    // For backups (overview §3, Backups): every file in data/, dot-names
    // included, copied into a backup's folder, or back from one (removing
    // what isn't in it). What `skip` names is left alone.
    Status copyOut(const std::string& hostFolder, const TreeSkip& skip) const;
    Status copyIn(const std::string& hostFolder, const TreeSkip& skip) const;

    // Removes everything in data/ except what `skip` names (dev mode's
    // "reset dev data").
    Status clear(const TreeSkip& skip) const;

private:
    Fd fd_;
    std::string hostPath_;
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
    // Removes a file; NotFound if there's none.
    Status remove(const RelPath& path) const;
    // The lock file for `key`, in private/state/locks/.
    Result<Fd> lockFile(std::string_view key) const;

    // The host path of a file here ("…/private/state/state.db"), for SQLite
    // only. The file's folder is created if missing.
    Result<std::string> sqlitePath(const RelPath& path) const;
    // The host path of folder `name` here ("backups"), created if missing:
    // for backups, which are written by host path.
    Result<std::string> folderPath(const RelPath& name) const;

    // For backups, as DataDir's.
    Status copyOut(const std::string& hostFolder, const TreeSkip& skip) const;
    Status copyIn(const std::string& hostFolder, const TreeSkip& skip) const;

    // The folders and files in `folder`, as DataDir::list.
    Result<std::vector<DirEntry>> list(const RelPath& folder) const;
    // Removes folder `folder` and everything in it; fine if there's none.
    Status removeTree(const RelPath& folder) const;

    // CGI mode's code snapshots (overview §8): a copy of code/ that a
    // compiled version serves from, so each request sees the code exactly as
    // it compiled, whatever is being uploaded meanwhile.
    //
    // Copies the files `listing` names (as `code` lists them) into folder
    // `to` here, which is emptied first, keeping their modification times.
    // A file gone since the listing is left out.
    Status saveCodeSnapshot(const CodeTree& code, const CodeListing& listing, const RelPath& to) const;
    // A read-only view of a snapshot saveCodeSnapshot made: the CodeTree the
    // compiler and the engine read, in place of code/.
    Result<CodeTree> openCodeSnapshot(const RelPath& folder, CodeTreeLimits limits = {}) const;

private:
    Fd fd_;
    std::string hostPath_;
};

}  // namespace fourfold

// Writing inside a partition, shared by DataDir, Dropbox and StateDir. The
// same rules as fs_util.h: one segment at a time, relative to the folder
// above, never following a symbolic link.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "base/fd.h"
#include "base/result.h"
#include "partition/data_dir.h"
#include "partition/paths.h"

namespace fourfold::writable {

// Opens folder `dir` below `rootFd`, creating missing folders (with `mode`).
Result<Fd> ensureFolder(int rootFd, const RelPath& dir, mode_t mode, const std::string& partition);

// The same, for the engine's own folders, whose names RelPath refuses
// (".fourfold/locks"). `segments` are trusted engine constants.
Result<Fd> ensureEngineFolder(int rootFd, const std::vector<std::string>& segments, mode_t mode,
                              const std::string& partition);

// The whole file, or TooLarge if it's over `maxBytes`.
Result<std::string> readFile(int rootFd, const RelPath& path, size_t maxBytes, const std::string& partition);

// Replaces `path` with `bytes` all at once: written to a temporary file in
// the same folder, flushed to disk, then renamed over it. A reader sees the
// old contents or the new, never part of either, even if the process dies
// part way. Creates missing folders.
Status writeAtomic(int rootFd, const RelPath& path, std::string_view bytes, mode_t mode,
                   const std::string& partition);

// Like writeAtomic, but only if `path` doesn't exist yet: Conflict if it
// does. Of several processes racing to create a file, exactly one wins.
Status createAtomic(int rootFd, const RelPath& path, std::string_view bytes, mode_t mode,
                    const std::string& partition);

// Appends `bytes` with one write, creating the file (and folders) if needed.
// Concurrent appends from any process don't interleave on local file systems.
Status append(int rootFd, const RelPath& path, std::string_view bytes, mode_t mode,
              const std::string& partition);

// Removes a file. NotFound if there's none.
Status removeFile(int rootFd, const RelPath& path, const std::string& partition);

// Whether `path` is a regular file.
Result<bool> fileExists(int rootFd, const RelPath& path, const std::string& partition);

// Creates folder `dir` and any missing folders above it. Fine if it exists;
// Conflict if something else is in the way.
Status makeFolder(int rootFd, const RelPath& dir, mode_t mode, const std::string& partition);

// Removes an empty folder: NotFound if there's none, Conflict if it isn't
// empty. Never the root.
Status removeFolder(int rootFd, const RelPath& dir, const std::string& partition);

// A file's or folder's kind, size and time, or nullopt if there's neither
// (a link or special file counts as neither). The root is a folder.
Result<std::optional<FileInfo>> info(int rootFd, const RelPath& path, const std::string& partition);

// The files and folders in `dir`, sorted by name. Leaves out dot-names
// (temporary files, .fourfold/), links, and anything else that isn't a
// regular file or a folder. A missing folder lists as empty.
Result<std::vector<DirEntry>> list(int rootFd, const RelPath& dir, const std::string& partition);

// Opens (creating) file `name` in an engine folder, for locking.
Result<Fd> openLockFile(int folderFd, const std::string& name, const std::string& what);

}  // namespace fourfold::writable

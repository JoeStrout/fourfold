// File access inside a partition, shared by the partition types.
//
// Every open goes one segment at a time, relative to the folder above it,
// and refuses symbolic links (O_NOFOLLOW). That keeps a path inside its
// partition even if someone swaps a folder for a link while we're looking:
// there's no gap between checking a path and opening it.
#pragma once

#include <string>
#include <sys/stat.h>

#include "base/fd.h"
#include "base/result.h"
#include "partition/paths.h"

namespace fourfold {

// An Error for a failed file operation on `what` (a partition-relative path
// for messages, like "code/pages/x.html"; never a host path). Missing files,
// symbolic links, and non-regular files are all NotFound: callers that face
// app code must not reveal more than "not found" (see raylib-miniscript's
// notes/SANDBOXING.md, rule 6). The message says which, for the developer.
Error fileError(int err, const std::string& what);

// Opens folder `dir` below `rootFd` (the root itself if `dir` is empty).
Result<Fd> openFolder(int rootFd, const RelPath& dir, const std::string& partition);

// Opens regular file `path` below `rootFd` for reading, and stats it.
Result<Fd> openFileForRead(int rootFd, const RelPath& path, const std::string& partition,
                           struct stat* info);

// Nanoseconds since the epoch.
int64_t modifiedTimeNs(const struct stat& info);

// `partition` + "/" + `path`, for messages.
std::string describe(const std::string& partition, const RelPath& path);

}  // namespace fourfold

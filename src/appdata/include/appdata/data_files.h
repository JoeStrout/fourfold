// DataFiles: the app's Tier 1 data, as files in data/ (overview §3; Arch §5
// appdata). What the script module `data` does, in C++.
//
// A name like "guestbook" or "polls/lunch" holds either
//   - a value: data/guestbook.json, one canonical JSON document, replaced
//     whole by set (atomically: readers see the old value or the new); or
//   - a log: data/guestbook.jsonl, one JSON value per line, added to by
//     append without any lock, and read back as a list.
// Using one kind of operation on the other kind is an error, not a silent
// conversion.
//
// Everything stored is inert (a Datum), and everything read is parsed as
// data, never run. The methods block on the disk: they're for worker
// threads. Read-modify-write operations (increment, compareAndSet, and the
// script module's update) must hold the name's lock from locks(), which also
// excludes other processes (CGI mode).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/datum.h"
#include "base/result.h"
#include "partition/data_dir.h"
#include "platform/worker_pool.h"
#include "state/lock_service.h"

namespace fourfold {

struct DataLimits {
    // The largest value or log, as stored.
    size_t maxBytes = 16 * 1024 * 1024;
};

class DataFiles {
public:
    explicit DataFiles(DataDir dir, DataLimits limits = {});
    ~DataFiles();

    DataFiles(const DataFiles&) = delete;
    DataFiles& operator=(const DataFiles&) = delete;

    // Checks a name from app code: '/'-separated segments under RelPath's
    // rules (no "..", no dot-names), and not ending in ".json" or ".jsonl"
    // (the extension is added here). Invalid, with the reason, if not.
    static Result<RelPath> checkName(std::string_view name);
    // A folder for names(): the same rules, and "" for data/ itself.
    static Result<RelPath> checkFolder(std::string_view folder);

    // The value or log (as a list); nullopt if there's neither.
    Result<std::optional<Datum>> get(const RelPath& name) const;
    Status set(const RelPath& name, const Datum& value) const;
    Status append(const RelPath& name, const Datum& value) const;
    // Whether there was anything to remove.
    Result<bool> remove(const RelPath& name) const;
    Result<bool> exists(const RelPath& name) const;
    // The names in a folder (values and logs, without their extensions), and
    // its subfolders (with a trailing '/'), sorted.
    Result<std::vector<std::string>> names(const RelPath& folder) const;

    // The lock for a name: hold it around a read-modify-write.
    LockService& locks() const { return *locks_; }
    static std::string lockKey(const RelPath& name) { return "data:" + name.str(); }

    // With the name's lock held: adds `by` to a number (a missing value
    // counts as 0), and returns the new number.
    Result<double> increment(const RelPath& name, double by) const;
    // With the name's lock held: replaces the value with `replacement` if it
    // equals `expected` (null matches a missing value). Returns whether it did.
    Result<bool> compareAndSet(const RelPath& name, const Datum& expected, const Datum& replacement) const;

private:
    RelPath valuePath(const RelPath& name) const;
    RelPath logPath(const RelPath& name) const;
    Result<std::optional<Datum>> readValue(const RelPath& name) const;

    DataDir dir_;
    DataLimits limits_;
    std::unique_ptr<WorkerPool> lockWaiters_;   // threads that wait on lock files
    std::unique_ptr<LockService> locks_;
};

}  // namespace fourfold

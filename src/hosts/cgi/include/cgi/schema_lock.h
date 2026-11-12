// SchemaFileLock: the schema lock (overview §3, Applying migrations) between
// processes, for CGI mode, where every request is a process of its own
// (M10). The standalone server's is SchemaGate, in memory.
//
// Two lock files in private/state/locks/. A request takes "schema-gate"
// shared, then "schema" shared, then lets the gate go, and holds "schema"
// until its response is made. Migrating takes the gate exclusively first,
// which holds new requests back (flock doesn't favor a waiting exclusive
// holder, so without the gate a busy site could keep "schema" shared
// forever), then "schema" exclusively, which waits for the requests already
// inside to finish.
#pragma once

#include <chrono>
#include <mutex>
#include <optional>

#include "base/result.h"
#include "partition/data_dir.h"
#include "partition/file_lock.h"
#include "pipeline/schema_gate.h"

namespace fourfold {

class SchemaFileLock {
public:
    // A request's hold on the lock, until destroyed.
    struct Shared {
        FileLock schema;
    };
    // Migrating's hold, until destroyed.
    struct Exclusive {
        FileLock gate;
        FileLock schema;
    };

    explicit SchemaFileLock(const StateDir& state) : state_(state) {}

    // For a request: nullopt if migrations have held it back for `wait`
    // (the request gets a 503).
    Result<std::optional<Shared>> enter(std::chrono::milliseconds wait) const;
    // For migrating: holds new requests back, and waits for those inside.
    Result<Exclusive> exclusive() const;

private:
    const StateDir& state_;
};

// The schema lock as a SchemaGate, for code written against one (the job
// runner, in `fourfold run-jobs`): while anything is inside, this process
// holds the file lock shared. enter() blocks the caller while it waits.
class FileSchemaGate : public SchemaGate {
public:
    FileSchemaGate(const SchemaFileLock& lock, std::chrono::milliseconds wait) : lock_(lock), wait_(wait) {}

    void enter(Admitted admitted) override;
    void leave() override;

private:
    const SchemaFileLock& lock_;
    std::chrono::milliseconds wait_;
    std::mutex mutex_;
    size_t inside_ = 0;
    std::optional<SchemaFileLock::Shared> held_;
};

}  // namespace fourfold

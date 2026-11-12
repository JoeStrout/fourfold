// JobQueue: background jobs waiting, running and dead, and the schedule's
// state (overview §2, Background jobs; Arch §5 state), in private/state/,
// where app code can't read or rewrite them.
//
// A job is a name and its arguments (as JSON, checked inert before they get
// here). Claiming one marks it running and counts the attempt; a job still
// marked running when the engine starts was interrupted, and goes back in
// the queue (at-least-once delivery). A job with a key is unique while it's
// queued: enqueuing the same key again adds nothing.
//
// Every method blocks on SQLite, so it's for worker threads (or the job
// runner's own); the store serializes its own calls.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/result.h"
#include "platform/sqlite.h"

namespace fourfold {

struct NewJob {
    std::string name;
    std::string argsJson = "{}";
    std::optional<std::string> key;
    int64_t runAtMs = 0;    // not before; 0: now
    std::string source;     // who enqueued it, for the dev tools ("POST /signup", "schedule", "job digest")
};

struct QueuedJob {
    enum class State { Queued, Running, Dead };

    int64_t id = 0;
    std::string name;
    std::string argsJson;
    std::optional<std::string> key;
    State state = State::Queued;
    int attempts = 0;        // started so far (a claimed job's includes this one)
    int64_t runAtMs = 0;
    int64_t createdMs = 0;
    int64_t startedMs = 0;   // the last attempt's start, if any
    std::string lastError;
    std::string source;
};

const char* jobStateName(QueuedJob::State state);   // "queued", "running", "dead"

// One finished attempt, for the dev tools' "recent runs".
struct JobRun {
    int64_t jobId = 0;
    std::string name;
    int attempt = 0;
    int64_t startedMs = 0;
    int64_t finishedMs = 0;
    std::string outcome;    // "ok", "retry", "dead", "dropped"
    std::string error;
};

// Where a scheduled job stands: when it last ran, and when it's next due.
struct ScheduleState {
    std::string name;
    std::string spec;         // the schedule's text, as of nextRunMs (a change recomputes it)
    int64_t lastRunMs = 0;    // 0: never
    int64_t nextRunMs = 0;
};

class JobQueue {
public:
    // Opens (creating) the tables in the database at `path`
    // (StateDir::sqlitePath). The clock must outlive the queue.
    static Result<std::unique_ptr<JobQueue>> open(const std::string& path, const Clock& clock = systemClock());

    // Adds jobs, all in one transaction (a request's or a job's enqueues
    // commit together). Returns how many were added: a job whose key is
    // already queued isn't.
    Result<size_t> enqueue(const std::vector<NewJob>& jobs);

    // The queued job due soonest, if it's due now: marked running, its
    // attempt counted.
    Result<std::optional<QueuedJob>> claim();
    // When the next queued job is due (ms), if there is one.
    Result<std::optional<int64_t>> nextDue();

    // A claimed job finished. On success it's removed. On failure it's
    // queued again at `retryAtMs`, or, without one, marked dead with its
    // error (a queued job with the same key takes its place instead). Either
    // way the attempt is recorded in the recent runs.
    Status succeed(int64_t id);
    Status fail(int64_t id, const std::string& error, std::optional<int64_t> retryAtMs);

    // Jobs left running by an engine that stopped: queued again, now.
    Result<int64_t> requeueRunning();

    // For the dev tools: jobs (queued, running and dead, the newest dead
    // last), and the latest finished attempts (newest first).
    Result<std::vector<QueuedJob>> list(size_t limit = 200);
    Result<std::vector<JobRun>> recentRuns(size_t limit = 50);
    // A dead job queued again (attempts from zero), or removed.
    Status retry(int64_t id);
    Status discard(int64_t id);
    // Whether a job with `key` is queued or running.
    Result<bool> pending(const std::string& key);
    // Names of jobs queued or running.
    Result<std::vector<std::string>> pendingNames();

    // The schedule's state, by job name.
    Result<std::map<std::string, ScheduleState>> schedules();
    Status saveSchedule(const ScheduleState& state);
    Status removeSchedule(const std::string& name);

private:
    JobQueue(std::unique_ptr<Sqlite> db, const Clock& clock) : db_(std::move(db)), clock_(clock) {}
    Status recordRun(const QueuedJob& job, const std::string& outcome, const std::string& error);
    Result<std::optional<QueuedJob>> find(int64_t id);

    std::mutex mutex_;
    std::unique_ptr<Sqlite> db_;
    const Clock& clock_;
};

}  // namespace fourfold

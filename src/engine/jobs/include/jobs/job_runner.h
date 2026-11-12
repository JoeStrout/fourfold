// JobRunner: the schedule and the job worker loop (overview §2, Background
// jobs; Arch §5 jobs).
//
// On its own thread, it:
// - fires code/schedule.ms's jobs when they're due, by queueing them (with
//   the key "schedule:<name>", so a tick is skipped while the last one is
//   still waiting or running: a scheduled job never overlaps itself). A
//   time missed while the engine was down is due at once, and runs once.
// - claims queued jobs that are due, at most config.jobs.workers at once,
//   and runs each in a fresh VM on whichever version of the app is current
//   when it starts. One that succeeds is removed, and what it enqueued and
//   published is committed; one that fails is tried again later (15 s, 30
//   s, 1 min, ...), until config.jobs.maxAttempts, and is then dead.
// - on start, queues again the jobs an engine left running when it stopped.
//
// Jobs hold the schema lock while they run, like requests, so migrations
// wait for them.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "base/clock.h"
#include "base/datum.h"
#include "observer/observer.h"
#include "pipeline/engine.h"
#include "pipeline/schema_gate.h"
#include "platform/event_loop.h"
#include "script/script_host.h"
#include "state/engine_log.h"
#include "state/job_queue.h"

namespace fourfold {

struct JobRunnerOptions {
    // The first retry's delay; each later one doubles it (at most an hour).
    std::chrono::milliseconds retryDelay{15'000};
    // The longest the runner sleeps between looks (it's woken sooner when
    // jobs are enqueued). Covers clock changes and other processes.
    std::chrono::milliseconds maxSleep{30'000};
    // Start with the schedule (and the queue) paused (the dev tools' toggle).
    bool paused = false;
    // Where to say what's happening (dev mode's terminal); may be null.
    std::function<void(const std::string&)> say;
};

class JobRunner {
public:
    // Everything given must outlive the runner. `schema` and `log` may be
    // null.
    static Result<std::unique_ptr<JobRunner>> create(JobQueue& queue, Engine& engine, ScriptHost& scripts,
                                                     SchemaGate* schema, EngineLog* log, const Clock& clock,
                                                     JobRunnerOptions options = {});
    // Stops: no new jobs start; runs in progress are abandoned (they'll be
    // queued again at the next start).
    ~JobRunner();

    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    // Starts looking (requeues interrupted jobs first). Call once.
    void start();
    // Look now: jobs were enqueued, or the app changed. Thread-safe.
    void wake();

    // --- The dev tools' jobs panel. Thread-safe. ---

    // {"paused", "running": n, "jobs": [...], "runs": [...], "schedule":
    // [{job, schedule, next, last}], "known": [names], "timeZone"}
    Datum status();
    // Queues `job` now, with no arguments ("run now"). False if there's no
    // such job.
    Result<bool> runNow(const std::string& job);
    Status retry(int64_t id);
    Status discard(int64_t id);
    void setPaused(bool paused);
    bool paused() const;
    // Jobs running now.
    size_t running() const;

    struct Core;   // private: shared with runs in progress

private:
    explicit JobRunner(std::shared_ptr<Core> core) : core_(std::move(core)) {}
    std::shared_ptr<Core> core_;
};

// The delay before attempt `attempt` + 1, after `attempt` failed: `first`,
// doubled for each later attempt, at most an hour.
std::chrono::milliseconds retryDelay(int attempt, std::chrono::milliseconds first);

// "2026-11-12 03:00 MST": a time as shown in the jobs panel, in `zone`.
std::string formatLocalTime(int64_t utcMillis, const TimeZone& zone);

}  // namespace fourfold

#include "jobs/job_runner.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <set>

namespace fourfold {

namespace {

constexpr auto kMaxRetryDelay = std::chrono::hours(1);

std::string describeDelay(std::chrono::milliseconds d) {
    int64_t s = d.count() / 1000;
    if (s < 120) return std::to_string(s) + " s";
    if (s < 7200) return std::to_string(s / 60) + " min";
    return std::to_string(s / 3600) + " h";
}

std::string located(const ScriptError& e) {
    std::string where = e.file.empty() ? "" : e.file + (e.line ? ":" + std::to_string(e.line) : "") + ": ";
    return where + e.message;
}

Datum str(const std::string& s) { return Datum::string(s); }
Datum num(double n) { return Datum::number(n); }

}  // namespace

std::chrono::milliseconds retryDelay(int attempt, std::chrono::milliseconds first) {
    std::chrono::milliseconds d = first;
    for (int i = 1; i < attempt && d < kMaxRetryDelay; i++) d *= 2;
    return std::min<std::chrono::milliseconds>(d, kMaxRetryDelay);
}

std::string formatLocalTime(int64_t utcMillis, const TimeZone& zone) {
    if (utcMillis <= 0 || utcMillis == INT64_MAX) return "";
    CivilTime t = civilFromSeconds(zone.toLocal(utcMillis / 1000));
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute,
                  t.second);
    return std::string(buffer) + " " + zone.name();
}

struct JobRunner::Core : std::enable_shared_from_this<Core> {
    JobQueue& queue;
    Engine& engine;
    ScriptHost& scripts;
    SchemaGate* schema;
    EngineLog* log;
    const Clock& clock;
    JobRunnerOptions options;

    std::unique_ptr<EventLoop> loop;
    std::thread thread;
    std::atomic<bool> paused{false};
    std::atomic<size_t> runningCount{0};

    // The runner's thread only.
    EventLoop::TimerId timer = 0;
    bool tickPosted = false;
    uint64_t checkedGeneration = 0;

    // Jobs running now, by id, for the panel. Any thread (under the mutex).
    std::mutex runningMutex;
    std::map<int64_t, std::string> running;

    Core(JobQueue& q, Engine& e, ScriptHost& s, SchemaGate* g, EngineLog* l, const Clock& c, JobRunnerOptions o)
        : queue(q), engine(e), scripts(s), schema(g), log(l), clock(c), options(std::move(o)) {}

    void say(const std::string& line) {
        if (options.say) options.say(line);
    }
    void note(const std::string& line) {
        if (log) log->write("jobs", line);
    }

    // Any thread: a tick soon, on the runner's thread.
    void wake() {
        std::weak_ptr<Core> weak = weak_from_this();
        loop->post([weak] {
            auto core = weak.lock();
            if (!core || core->tickPosted) return;
            core->tickPosted = true;
            core->loop->post([weak] {
                if (auto c = weak.lock()) {
                    c->tickPosted = false;
                    c->tick();
                }
            });
        });
    }

    void tick() {
        int64_t now = clock.nowMillis();
        int64_t nextLook = now + options.maxSleep.count();
        Engine::JobServices current = engine.jobServices();
        if (current.app) {
            nextLook = std::min(nextLook, fireSchedule(*current.app, *current.config, now));
            checkRemovedJobs(*current.app);
        }
        size_t workers = current.config ? current.config->jobs.workers : 1;
        while (runningCount < workers) {
            auto claimed = queue.claim();
            if (!claimed) {
                note("claiming a job failed: " + claimed.error().message);
                break;
            }
            if (!claimed.value()) break;
            startJob(std::move(*claimed.value()));
        }
        if (auto due = queue.nextDue(); due && due.value()) nextLook = std::min(nextLook, *due.value());
        if (timer) loop->cancelTimer(timer);
        std::weak_ptr<Core> weak = weak_from_this();
        auto delay = std::chrono::milliseconds(std::max<int64_t>(nextLook - clock.nowMillis(), 0) + 5);
        timer = loop->addTimer(delay, [weak] {
            if (auto c = weak.lock()) {
                c->timer = 0;
                c->tick();
            }
        });
    }

    // Queues the scheduled jobs that are due; returns when the next one is.
    int64_t fireSchedule(const CompiledApp& app, const AppConfig& config, int64_t now) {
        int64_t next = INT64_MAX;
        auto states = queue.schedules();
        if (!states) {
            note("reading the schedule's state failed: " + states.error().message);
            return now + options.maxSleep.count();
        }
        const TimeZone& zone = *config.jobs.timeZone;
        std::set<std::string> scheduled;
        for (const ScheduledJob& sj : app.schedule) {
            scheduled.insert(sj.job);
            const Schedule& schedule = sj.schedule;
            std::string spec = schedule.text + " @ " + zone.name();
            auto found = states.value().find(sj.job);
            ScheduleState st;
            bool changed = false;
            if (found == states.value().end() || found->second.spec != spec) {
                // New, or its schedule or the zone changed: when it's due now.
                st.name = sj.job;
                st.spec = spec;
                if (found != states.value().end()) st.lastRunMs = found->second.lastRunMs;
                if (schedule.kind == Schedule::Kind::Every) {
                    st.nextRunMs = st.lastRunMs ? st.lastRunMs + schedule.everySeconds * 1000 : now;
                } else {
                    st.nextRunMs = nextTime(schedule, now, zone);
                }
                changed = true;
            } else {
                st = found->second;
            }
            if (!paused && st.nextRunMs <= now) {
                // Due (or overdue, after downtime: it runs once).
                std::string key = "schedule:" + sj.job;
                auto pending = queue.pending(key);
                if (pending && pending.value()) {
                    note("scheduled job " + sj.job + " skipped: the last run is still waiting or running");
                } else if (pending) {
                    NewJob job;
                    job.name = sj.job;
                    job.key = key;
                    job.source = "schedule (" + schedule.describe() + ")";
                    if (auto added = queue.enqueue({job}); !added) {
                        note("queueing scheduled job " + sj.job + " failed: " + added.error().message);
                    }
                    st.lastRunMs = now;
                }
                st.nextRunMs = schedule.kind == Schedule::Kind::Every ? now + schedule.everySeconds * 1000
                                                                      : nextTime(schedule, now, zone);
                changed = true;
            }
            if (changed) {
                if (Status s = queue.saveSchedule(st); !s) note("saving the schedule failed: " + s.error().message);
            }
            if (!paused) next = std::min(next, st.nextRunMs);
        }
        for (const auto& [name, st] : states.value()) {
            if (!scheduled.count(name)) (void)queue.removeSchedule(name);
        }
        return next;
    }

    static int64_t nextTime(const Schedule& s, int64_t nowMs, const TimeZone& zone) {
        int64_t next = s.nextAfter(nowMs / 1000, zone);
        return next == INT64_MAX ? INT64_MAX : next * 1000;
    }

    // After a deploy: jobs still queued that the new version doesn't have.
    void checkRemovedJobs(const CompiledApp& app) {
        if (app.generation == checkedGeneration) return;
        checkedGeneration = app.generation;
        auto names = queue.pendingNames();
        if (!names) return;
        for (const std::string& name : names.value()) {
            if (app.jobs.count(name)) continue;
            std::string line = "job " + name + " is queued, but this version of the app has no job." + name +
                               "; its entries will go dead when they come up";
            note(line);
            say("Warning: " + line + ".");
        }
    }

    void startJob(QueuedJob job) {
        Engine::JobServices services = engine.jobServices();
        auto ref = services.app ? services.app->jobs.find(job.name) : decltype(services.app->jobs.end()){};
        if (!services.app || ref == services.app->jobs.end()) {
            std::string why = "there's no job named " + job.name + " in this version of the app";
            (void)queue.fail(job.id, why, std::nullopt);
            note("job " + job.name + " is dead: " + why);
            say("Job " + job.name + " is dead: " + why + ".");
            return;
        }
        auto args = parseJson(job.argsJson);
        if (!args || args.value().kind() != Datum::Kind::Map) {
            (void)queue.fail(job.id, "its arguments couldn't be read", std::nullopt);
            return;
        }
        HandlerRef handler = ref->second;
        runningCount++;
        {
            std::lock_guard<std::mutex> lock(runningMutex);
            running[job.id] = job.name;
        }
        std::weak_ptr<Core> weak = weak_from_this();
        SchemaGate* gate = schema;
        ScriptHost* host = &scripts;
        auto shared = std::make_shared<QueuedJob>(std::move(job));
        auto argsShared = std::make_shared<Datum>(std::move(args).value());
        auto servicesShared = std::make_shared<Engine::JobServices>(std::move(services));
        auto run = [weak, gate, host, shared, argsShared, servicesShared, handler] {
            auto started = std::chrono::steady_clock::now();
            host->runJob(servicesShared->app, handler, *argsShared, servicesShared->context,
                         [weak, gate, shared, started](HandlerOutcome outcome) {
                             if (gate) gate->leave();
                             auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started);
                             auto core = weak.lock();
                             if (!core) return;
                             auto result = std::make_shared<HandlerOutcome>(std::move(outcome));
                             core->loop->post([weak, shared, result, ms] {
                                 if (auto c = weak.lock()) c->finishJob(*shared, *result, ms);
                             });
                         });
        };
        if (!gate) {
            run();
            return;
        }
        // Inside the schema lock, like a request: migrations wait for jobs.
        gate->enter([weak, run, shared](bool admitted) {
            if (admitted) {
                run();
                return;
            }
            if (auto core = weak.lock()) {
                core->loop->post([weak, shared] {
                    if (auto c = weak.lock()) {
                        HandlerOutcome waited;
                        waited.kind = HandlerOutcome::Kind::Error;
                        waited.error.message = "waited too long while migrations ran";
                        c->finishJob(*shared, waited, std::chrono::milliseconds(0));
                    }
                });
            }
        });
    }

    void finishJob(const QueuedJob& job, HandlerOutcome outcome, std::chrono::milliseconds took) {
        std::string source = "job " + job.name;
        if (outcome.kind != HandlerOutcome::Kind::Error) {
            if (Status s = engine.commitEffects(outcome, source); !s) {
                outcome.kind = HandlerOutcome::Kind::Error;
                outcome.error = ScriptError{s.error().message, "", 0};
            }
        } else {
            engine.dropEffects(outcome.droppedJobs, source);
        }
        const int maxAttempts = engine.config()->jobs.maxAttempts;
        if (outcome.kind != HandlerOutcome::Kind::Error) {
            if (Status s = queue.succeed(job.id); !s) note("finishing job " + job.name + " failed: " + s.error().message);
            note("job " + job.name + " ran in " + std::to_string(took.count()) + " ms");
            say("Job " + job.name + " ran (" + std::to_string(took.count()) + " ms).");
        } else {
            std::string error = located(outcome.error);
            std::optional<int64_t> retryAt;
            std::string next;
            if (job.attempts < maxAttempts) {
                auto delay = retryDelay(job.attempts, options.retryDelay);
                retryAt = clock.nowMillis() + delay.count();
                next = "trying again in " + describeDelay(delay);
            } else {
                next = "it's dead after " + std::to_string(job.attempts) +
                       (job.attempts == 1 ? " attempt" : " attempts") + " (see /__dev/jobs)";
            }
            if (Status s = queue.fail(job.id, error, retryAt); !s) {
                note("recording job " + job.name + "'s failure failed: " + s.error().message);
            }
            std::string line = "job " + job.name + " failed (attempt " + std::to_string(job.attempts) + " of " +
                               std::to_string(maxAttempts) + "): " + error + "; " + next;
            note(line);
            say("Job " + job.name + " failed (attempt " + std::to_string(job.attempts) + " of " +
                std::to_string(maxAttempts) + "): " + error + "; " + next + ".");
        }
        {
            std::lock_guard<std::mutex> lock(runningMutex);
            running.erase(job.id);
        }
        runningCount--;
        tick();
    }
};

Result<std::unique_ptr<JobRunner>> JobRunner::create(JobQueue& queue, Engine& engine, ScriptHost& scripts,
                                                     SchemaGate* schema, EngineLog* log, const Clock& clock,
                                                     JobRunnerOptions options) {
    auto loop = EventLoop::create();
    if (!loop) return loop.error();
    auto core = std::make_shared<Core>(queue, engine, scripts, schema, log, clock, std::move(options));
    core->paused = core->options.paused;
    core->loop = std::move(loop).value();
    return std::unique_ptr<JobRunner>(new JobRunner(std::move(core)));
}

JobRunner::~JobRunner() {
    if (!core_) return;
    core_->loop->stop();
    if (core_->thread.joinable()) core_->thread.join();
}

void JobRunner::start() {
    Core* core = core_.get();
    if (auto requeued = core->queue.requeueRunning(); requeued && requeued.value() > 0) {
        core->note(std::to_string(requeued.value()) + (requeued.value() == 1 ? " job was" : " jobs were") +
                   " interrupted when the engine stopped; queued again");
    }
    core->thread = std::thread([core] { core->loop->run(); });
    core->wake();
}

void JobRunner::wake() { core_->wake(); }

void JobRunner::setPaused(bool paused) {
    core_->paused = paused;
    core_->note(paused ? "the schedule is paused" : "the schedule is running again");
    core_->wake();
}

bool JobRunner::paused() const { return core_->paused; }

size_t JobRunner::running() const { return core_->runningCount; }

Result<bool> JobRunner::runNow(const std::string& job) {
    std::shared_ptr<const CompiledApp> app = core_->engine.app();
    if (!app || !app->jobs.count(job)) return false;
    NewJob n;
    n.name = job;
    n.source = "run now (dev tools)";
    auto added = core_->queue.enqueue({n});
    if (!added) return added.error();
    core_->wake();
    return true;
}

Status JobRunner::retry(int64_t id) {
    Status s = core_->queue.retry(id);
    core_->wake();
    return s;
}

Status JobRunner::discard(int64_t id) { return core_->queue.discard(id); }

Datum JobRunner::status() {
    Core& c = *core_;
    std::shared_ptr<const AppConfig> config = c.engine.config();
    std::shared_ptr<const CompiledApp> app = c.engine.app();
    const TimeZone& zone = *config->jobs.timeZone;
    Datum::ListItems jobs, runs, schedule, known;
    if (auto list = c.queue.list()) {
        for (const QueuedJob& j : list.value()) {
            jobs.push_back(Datum::map({{"id", num(static_cast<double>(j.id))},
                                       {"name", str(j.name)},
                                       {"args", str(j.argsJson)},
                                       {"key", str(j.key.value_or(""))},
                                       {"state", str(jobStateName(j.state))},
                                       {"attempts", num(j.attempts)},
                                       {"runAt", str(formatLocalTime(j.runAtMs, zone))},
                                       {"created", str(formatLocalTime(j.createdMs, zone))},
                                       {"lastError", str(j.lastError)},
                                       {"source", str(j.source)}}));
        }
    }
    if (auto list = c.queue.recentRuns()) {
        for (const JobRun& r : list.value()) {
            runs.push_back(Datum::map({{"name", str(r.name)},
                                       {"attempt", num(r.attempt)},
                                       {"started", str(formatLocalTime(r.startedMs, zone))},
                                       {"ms", num(static_cast<double>(r.finishedMs - r.startedMs))},
                                       {"outcome", str(r.outcome)},
                                       {"error", str(r.error)}}));
        }
    }
    if (app) {
        auto states = c.queue.schedules();
        for (const ScheduledJob& sj : app->schedule) {
            ScheduleState st;
            if (states) {
                auto it = states.value().find(sj.job);
                if (it != states.value().end()) st = it->second;
            }
            schedule.push_back(Datum::map({{"job", str(sj.job)},
                                           {"schedule", str(sj.schedule.text)},
                                           {"next", str(c.paused ? "(paused)" : formatLocalTime(st.nextRunMs, zone))},
                                           {"last", str(formatLocalTime(st.lastRunMs, zone))}}));
        }
        for (const auto& [name, ref] : app->jobs) {
            known.push_back(Datum::map({{"name", str(name)}, {"file", str("code/" + ref.source.str())}}));
        }
    }
    return Datum::map({{"paused", num(c.paused ? 1 : 0)},
                       {"running", num(static_cast<double>(c.runningCount.load()))},
                       {"workers", num(static_cast<double>(config->jobs.workers))},
                       {"timeZone", str(zone.name())},
                       {"jobs", Datum::list(std::move(jobs))},
                       {"runs", Datum::list(std::move(runs))},
                       {"schedule", Datum::list(std::move(schedule))},
                       {"known", Datum::list(std::move(known))}});
}

}  // namespace fourfold

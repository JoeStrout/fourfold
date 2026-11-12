#include "state/job_queue.h"

namespace fourfold {

namespace {

const char* const kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    args TEXT NOT NULL,
    key TEXT,
    state TEXT NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    run_at INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    started_at INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NOT NULL DEFAULT '',
    source TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS jobs_due ON jobs (state, run_at);
CREATE UNIQUE INDEX IF NOT EXISTS jobs_queued_key ON jobs (key) WHERE key IS NOT NULL AND state = 'queued';
CREATE TABLE IF NOT EXISTS job_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    started_at INTEGER NOT NULL,
    finished_at INTEGER NOT NULL,
    outcome TEXT NOT NULL,
    error TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS job_schedule (
    name TEXT PRIMARY KEY,
    spec TEXT NOT NULL,
    last_run INTEGER NOT NULL,
    next_run INTEGER NOT NULL
) WITHOUT ROWID;
)SQL";

// Finished attempts kept for the dev tools.
constexpr int64_t kKeepRuns = 200;

const char* const kColumns =
    "id, name, args, key, state, attempts, run_at, created_at, started_at, last_error, source";

QueuedJob::State stateFrom(const std::string& s) {
    return s == "running" ? QueuedJob::State::Running : s == "dead" ? QueuedJob::State::Dead : QueuedJob::State::Queued;
}

QueuedJob jobFrom(SqliteStatement& st) {
    QueuedJob j;
    j.id = st.integer(0);
    j.name = st.text(1);
    j.argsJson = st.text(2);
    if (!st.isNull(3)) j.key = st.text(3);
    j.state = stateFrom(st.text(4));
    j.attempts = static_cast<int>(st.integer(5));
    j.runAtMs = st.integer(6);
    j.createdMs = st.integer(7);
    j.startedMs = st.integer(8);
    j.lastError = st.text(9);
    j.source = st.text(10);
    return j;
}

}  // namespace

const char* jobStateName(QueuedJob::State state) {
    switch (state) {
        case QueuedJob::State::Queued: return "queued";
        case QueuedJob::State::Running: return "running";
        case QueuedJob::State::Dead: return "dead";
    }
    return "queued";
}

Result<std::unique_ptr<JobQueue>> JobQueue::open(const std::string& path, const Clock& clock) {
    auto db = Sqlite::open(path);
    if (!db) return db.error();
    if (Status s = db.value()->exec(kSchema); !s) return s.error();
    return std::unique_ptr<JobQueue>(new JobQueue(std::move(db).value(), clock));
}

Result<size_t> JobQueue::enqueue(const std::vector<NewJob>& jobs) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = clock_.nowMillis();
    size_t added = 0;
    Status s = db_->transaction([&]() -> Status {
        added = 0;
        for (const NewJob& j : jobs) {
            auto st = db_->statement(
                "INSERT OR IGNORE INTO jobs (name, args, key, state, run_at, created_at, source) "
                "VALUES (?1, ?2, ?3, 'queued', ?4, ?5, ?6)");
            if (!st) return st.error();
            st.value()->bindText(1, j.name).bindText(2, j.argsJson).bindTextOrNull(3, j.key);
            st.value()->bindInt(4, j.runAtMs > 0 ? j.runAtMs : now).bindInt(5, now).bindText(6, j.source);
            if (Status r = st.value()->run(); !r) return r;
            added += static_cast<size_t>(db_->changes());
        }
        return {};
    });
    if (!s) return s.error();
    return added;
}

Result<std::optional<QueuedJob>> JobQueue::claim() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = clock_.nowMillis();
    std::optional<QueuedJob> claimed;
    Status s = db_->transaction([&]() -> Status {
        claimed.reset();
        auto st = db_->statement(std::string("SELECT ") + kColumns +
                                 " FROM jobs WHERE state = 'queued' AND run_at <= ?1 ORDER BY run_at, id LIMIT 1");
        if (!st) return st.error();
        st.value()->bindInt(1, now);
        auto row = st.value()->step();
        if (!row) return row.error();
        if (!row.value()) return {};
        QueuedJob j = jobFrom(*st.value());
        st.value()->finish();
        auto update = db_->statement(
            "UPDATE jobs SET state = 'running', attempts = attempts + 1, started_at = ?2 WHERE id = ?1");
        if (!update) return update.error();
        update.value()->bindInt(1, j.id).bindInt(2, now);
        if (Status r = update.value()->run(); !r) return r;
        j.state = QueuedJob::State::Running;
        j.attempts++;
        j.startedMs = now;
        claimed = std::move(j);
        return {};
    });
    if (!s) return s.error();
    return claimed;
}

Result<std::optional<int64_t>> JobQueue::nextDue() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement("SELECT MIN(run_at) FROM jobs WHERE state = 'queued'");
    if (!st) return st.error();
    auto row = st.value()->step();
    if (!row) return row.error();
    std::optional<int64_t> out;
    if (row.value() && !st.value()->isNull(0)) out = st.value()->integer(0);
    st.value()->finish();
    return out;
}

Result<std::optional<QueuedJob>> JobQueue::find(int64_t id) {
    auto st = db_->statement(std::string("SELECT ") + kColumns + " FROM jobs WHERE id = ?1");
    if (!st) return st.error();
    st.value()->bindInt(1, id);
    auto row = st.value()->step();
    if (!row) return row.error();
    std::optional<QueuedJob> out;
    if (row.value()) out = jobFrom(*st.value());
    st.value()->finish();
    return out;
}

Status JobQueue::recordRun(const QueuedJob& job, const std::string& outcome, const std::string& error) {
    auto st = db_->statement(
        "INSERT INTO job_runs (job_id, name, attempt, started_at, finished_at, outcome, error) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (!st) return st.error();
    st.value()->bindInt(1, job.id).bindText(2, job.name).bindInt(3, job.attempts).bindInt(4, job.startedMs);
    st.value()->bindInt(5, clock_.nowMillis()).bindText(6, outcome).bindText(7, error);
    if (Status r = st.value()->run(); !r) return r;
    auto trim = db_->statement("DELETE FROM job_runs WHERE id <= (SELECT MAX(id) FROM job_runs) - ?1");
    if (!trim) return trim.error();
    trim.value()->bindInt(1, kKeepRuns);
    return trim.value()->run();
}

Status JobQueue::succeed(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->transaction([&]() -> Status {
        auto job = find(id);
        if (!job) return job.error();
        if (!job.value()) return {};
        auto st = db_->statement("DELETE FROM jobs WHERE id = ?1");
        if (!st) return st.error();
        st.value()->bindInt(1, id);
        if (Status r = st.value()->run(); !r) return r;
        return recordRun(*job.value(), "ok", "");
    });
}

Status JobQueue::fail(int64_t id, const std::string& error, std::optional<int64_t> retryAtMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->transaction([&]() -> Status {
        auto found = find(id);
        if (!found) return found.error();
        if (!found.value()) return {};
        const QueuedJob& job = *found.value();
        if (retryAtMs && job.key) {
            // A newer one with the same key is already waiting: it does the work.
            auto st = db_->statement("SELECT COUNT(*) FROM jobs WHERE key = ?1 AND state = 'queued'");
            if (!st) return st.error();
            st.value()->bindText(1, *job.key);
            auto row = st.value()->step();
            if (!row) return row.error();
            bool taken = row.value() && st.value()->integer(0) > 0;
            st.value()->finish();
            if (taken) {
                auto del = db_->statement("DELETE FROM jobs WHERE id = ?1");
                if (!del) return del.error();
                del.value()->bindInt(1, id);
                if (Status r = del.value()->run(); !r) return r;
                return recordRun(job, "dropped", error);
            }
        }
        auto st = db_->statement(retryAtMs ? "UPDATE jobs SET state = 'queued', run_at = ?2, last_error = ?3 WHERE id = ?1"
                                           : "UPDATE jobs SET state = 'dead', last_error = ?3 WHERE id = ?1");
        if (!st) return st.error();
        st.value()->bindInt(1, id).bindInt(2, retryAtMs.value_or(0)).bindText(3, error);
        if (Status r = st.value()->run(); !r) return r;
        return recordRun(job, retryAtMs ? "retry" : "dead", error);
    });
}

Result<int64_t> JobQueue::requeueRunning() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t changed = 0;
    Status s = db_->transaction([&]() -> Status {
        // A running job whose key is queued again meanwhile gives way to it.
        auto drop = db_->statement(
            "DELETE FROM jobs WHERE state = 'running' AND key IS NOT NULL AND key IN "
            "(SELECT key FROM jobs WHERE state = 'queued' AND key IS NOT NULL)");
        if (!drop) return drop.error();
        if (Status r = drop.value()->run(); !r) return r;
        auto st = db_->statement(
            "UPDATE jobs SET state = 'queued', run_at = ?1, last_error = 'interrupted (the engine stopped while it "
            "ran)' WHERE state = 'running'");
        if (!st) return st.error();
        st.value()->bindInt(1, clock_.nowMillis());
        if (Status r = st.value()->run(); !r) return r;
        changed = db_->changes();
        return {};
    });
    if (!s) return s.error();
    return changed;
}

Result<std::vector<QueuedJob>> JobQueue::list(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement(std::string("SELECT ") + kColumns +
                             " FROM jobs ORDER BY CASE state WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END, "
                             "run_at, id LIMIT ?1");
    if (!st) return st.error();
    st.value()->bindInt(1, static_cast<int64_t>(limit));
    std::vector<QueuedJob> out;
    for (;;) {
        auto row = st.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        out.push_back(jobFrom(*st.value()));
    }
    return out;
}

Result<std::vector<JobRun>> JobQueue::recentRuns(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement(
        "SELECT job_id, name, attempt, started_at, finished_at, outcome, error FROM job_runs ORDER BY id DESC "
        "LIMIT ?1");
    if (!st) return st.error();
    st.value()->bindInt(1, static_cast<int64_t>(limit));
    std::vector<JobRun> out;
    for (;;) {
        auto row = st.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        SqliteStatement& r = *st.value();
        out.push_back({r.integer(0), r.text(1), static_cast<int>(r.integer(2)), r.integer(3), r.integer(4), r.text(5),
                       r.text(6)});
    }
    return out;
}

Status JobQueue::retry(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement(
        "UPDATE OR IGNORE jobs SET state = 'queued', attempts = 0, run_at = ?2 WHERE id = ?1 AND state = 'dead'");
    if (!st) return st.error();
    st.value()->bindInt(1, id).bindInt(2, clock_.nowMillis());
    return st.value()->run();
}

Status JobQueue::discard(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement("DELETE FROM jobs WHERE id = ?1 AND state != 'running'");
    if (!st) return st.error();
    st.value()->bindInt(1, id);
    return st.value()->run();
}

Result<bool> JobQueue::pending(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement("SELECT COUNT(*) FROM jobs WHERE key = ?1 AND state IN ('queued', 'running')");
    if (!st) return st.error();
    st.value()->bindText(1, key);
    auto row = st.value()->step();
    if (!row) return row.error();
    bool out = row.value() && st.value()->integer(0) > 0;
    st.value()->finish();
    return out;
}

Result<std::vector<std::string>> JobQueue::pendingNames() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement("SELECT DISTINCT name FROM jobs WHERE state IN ('queued', 'running') ORDER BY name");
    if (!st) return st.error();
    std::vector<std::string> out;
    for (;;) {
        auto row = st.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        out.push_back(st.value()->text(0));
    }
    return out;
}

Result<std::map<std::string, ScheduleState>> JobQueue::schedules() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement("SELECT name, spec, last_run, next_run FROM job_schedule");
    if (!st) return st.error();
    std::map<std::string, ScheduleState> out;
    for (;;) {
        auto row = st.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        SqliteStatement& r = *st.value();
        out[r.text(0)] = {r.text(0), r.text(1), r.integer(2), r.integer(3)};
    }
    return out;
}

Status JobQueue::saveSchedule(const ScheduleState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement(
        "INSERT INTO job_schedule (name, spec, last_run, next_run) VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT (name) DO UPDATE SET spec = ?2, last_run = ?3, next_run = ?4");
    if (!st) return st.error();
    st.value()->bindText(1, state.name).bindText(2, state.spec).bindInt(3, state.lastRunMs).bindInt(4, state.nextRunMs);
    return st.value()->run();
}

Status JobQueue::removeSchedule(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto st = db_->statement("DELETE FROM job_schedule WHERE name = ?1");
    if (!st) return st.error();
    st.value()->bindText(1, name);
    return st.value()->run();
}

}  // namespace fourfold

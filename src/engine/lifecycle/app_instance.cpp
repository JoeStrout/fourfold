#include "lifecycle/app_instance.h"

#include <algorithm>
#include <cctype>
#include <future>
#include <set>

#include "compiler/compiler.h"

namespace fourfold {

namespace {

bool isStylesheet(const RelPath& path) {
    std::string ext(path.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".css";
}

}  // namespace

Result<std::unique_ptr<AppInstance>> AppInstance::create(CodeTree code, ScriptHost* scripts, Engine& engine,
                                                         Observer* observer, LifecycleOptions options,
                                                         DataServices* data) {
    auto loop = EventLoop::create();
    if (!loop) return loop.error();
    return std::unique_ptr<AppInstance>(
        new AppInstance(std::move(code), scripts, engine, observer, options, data, std::move(loop).value()));
}

AppInstance::AppInstance(CodeTree code, ScriptHost* scripts, Engine& engine, Observer* observer,
                         LifecycleOptions options, DataServices* data, std::unique_ptr<EventLoop> loop)
    : code_(std::move(code)),
      scripts_(scripts),
      engine_(engine),
      observer_(observer),
      options_(options),
      data_(data && data->data ? data : nullptr),
      loop_(std::move(loop)) {
    if (data_) {
        migrator_ = std::make_unique<Migrator>(
            data_->db, *data_->data,
            [this](const CompiledApp&, const Migration& m, DbConnection* tx) { return runMigrationScript(m, tx); });
        migrator_->setFaultInjector(options_.faults);
        if (data_->databaseExists) migrator_->setDatabaseExists(data_->databaseExists);
    }
    thread_ = std::thread([this] { loop_->run(); });
}

AppInstance::~AppInstance() {
    loop_->stop();
    thread_.join();
}

CompileReport AppInstance::start() {
    CompileReport report = recompileNow();
    loop_->post([this] {
        watching_ = true;
        schedulePoll();
    });
    return report;
}

CompileReport AppInstance::recompileNow() {
    std::promise<CompileReport> done;
    loop_->post([this, &done] {
        auto listed = snapshot();
        done.set_value(compileNow(listed ? *listed : Snapshot{}));
    });
    return done.get_future().get();
}

std::shared_ptr<const CompiledApp> AppInstance::current() const {
    std::lock_guard<std::mutex> lock(currentMutex_);
    return current_;
}

std::optional<AppInstance::Snapshot> AppInstance::snapshot() const {
    auto listing = code_.list();
    if (!listing) return std::nullopt;
    Snapshot s;
    for (const CodeEntry& e : listing.value().files) s[e.path] = {e.size, e.modifiedNs};
    return s;
}

void AppInstance::schedulePoll() {
    loop_->addTimer(options_.pollInterval, [this] { poll(); });
}

void AppInstance::pollConfig() {
    if (!options_.config || !options_.config->sources.dir || !scripts_) return;
    ConfigWatch& watch = *options_.config;
    std::string stamp = configStamp(*watch.sources.dir);
    if (stamp == watch.stamp) return;
    watch.stamp = stamp;
    ConfigLoad load = loadConfig(watch.sources, *scripts_);
    if (load.ok()) engine_.setConfig(load.config);
    if (watch.loaded) watch.loaded(load);
}

void AppInstance::poll() {
    schedulePoll();
    pollConfig();

    std::optional<int64_t> trigger = code_.deployTrigger();
    if (trigger && trigger != deployTrigger_) {
        auto listed = snapshot();
        compileNow(listed ? *listed : compiled_);
        return;
    }
    if (options_.trigger == LifecycleOptions::Trigger::DeployOnly) return;

    // A listing that fails (e.g. too many files) is a compile error, and the
    // compiler will say why; treat it as a change only once.
    auto listed = snapshot();
    Snapshot now = listed ? std::move(*listed) : Snapshot{};
    if (now == compiled_) {
        pending_.reset();
        return;
    }
    auto t = EventLoop::Clock::now();
    if (!pending_ || *pending_ != now) {
        pending_ = std::move(now);   // still changing; wait for quiet
        pendingSince_ = t;
        return;
    }
    if (t - pendingSince_ >= options_.quietPeriod) compileNow(*pending_);
}

CompileReport AppInstance::compileNow(const Snapshot& snapshot) {
    if (observer_) observer_->compileStarted();
    auto t0 = std::chrono::steady_clock::now();
    CompileReport report;

    // What changed since the last compile (for live reload).
    if (compiledOnce_) {
        bool onlyStylesheets = lastCompileOk_;
        for (const auto& [path, stamp] : snapshot) {
            auto old = compiled_.find(path);
            if (old != compiled_.end() && old->second == stamp) continue;
            report.changed.push_back(path);
            if (old == compiled_.end() || !isStylesheet(path)) onlyStylesheets = false;
        }
        for (const auto& [path, stamp] : compiled_) {
            if (snapshot.count(path)) continue;
            report.changed.push_back(path);
            onlyStylesheets = false;
        }
        std::sort(report.changed.begin(), report.changed.end());
        report.cssOnly = onlyStylesheets && !report.changed.empty();
    }
    compiled_ = snapshot;
    deployTrigger_ = code_.deployTrigger();
    pending_.reset();
    compiledOnce_ = true;

    CompileOptions options;
    options.generation = nextGeneration_;
    options.scripts = scripts_;
    options.evaluator = scripts_;
    // The database in use (a changed config.db takes a restart).
    options.dialect = data_ && data_->db ? data_->db->dialect() : engine_.config()->db.dialect;
    CompileOutput out = compile(code_, options);
    report.ok = out.ok();
    report.diagnostics = std::move(out.diagnostics);
    if (report.ok) {
        nextGeneration_++;
        newest_ = out.app;
        report.ok = install(std::move(out.app), report);
        if (!report.migrated.empty()) report.cssOnly = false;
    }
    lastCompileOk_ = report.ok;
    if (!report.ok) report.cssOnly = false;
    if (auto app = current()) report.generation = app->generation;
    report.duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    if (observer_) observer_->compileFinished(report);
    return report;
}

void AppInstance::swap(std::shared_ptr<const CompiledApp> app) {
    {
        std::lock_guard<std::mutex> lock(currentMutex_);
        current_ = app;
        waiting_.reset();
        waitingCount_ = 0;
    }
    engine_.setApp(std::move(app));   // the old version goes when its requests finish
}

bool AppInstance::install(std::shared_ptr<const CompiledApp> app, CompileReport& report) {
    if (!migrator_) {
        swap(std::move(app));
        return true;
    }
    auto applied = migrator_->applied();
    if (!applied) {
        std::string message = "can't read which migrations are applied: " + applied.error().message;
        report.diagnostics.push_back({Diagnostic::Severity::Error, "code/migrations", 0, message});
        problems_ = {message};
        return false;
    }
    MigrationPlan plan = planMigrations(*app, applied.value(), options_.allowOlderCode);
    problems_ = plan.errors;
    for (const std::string& e : plan.errors) {
        report.diagnostics.push_back({Diagnostic::Severity::Error, "code/migrations", 0, e});
    }
    for (const std::string& w : plan.warnings) {
        report.diagnostics.push_back({Diagnostic::Severity::Warning, "code/migrations", 0, w});
    }
    if (!plan.ok()) return false;
    if (plan.pending.empty()) {
        swap(std::move(app));
        return true;
    }
    if (!options_.autoMigrate) {
        std::lock_guard<std::mutex> lock(currentMutex_);
        waiting_ = std::move(app);
        waitingCount_ = plan.pending.size();
        report.pendingMigrations = plan.pending.size();
        return true;   // compiled; the old version serves until `fourfold migrate`
    }
    bool fresh = applied.value().empty();
    MigrateResult r = migrate(app, plan, options_.seed && fresh && app->seed.has_value());
    report.migrated = r.applied;
    report.backup = r.backup;
    report.seeded = r.ok && options_.seed && fresh && app->seed.has_value();
    if (!r.ok) {
        report.diagnostics.push_back({Diagnostic::Severity::Error, "code/migrations", 0, r.message});
        return false;
    }
    return true;
}

Status AppInstance::fault(const std::string& step) const {
    return options_.faults ? options_.faults(step) : Status{};
}

void AppInstance::hold() {
    if (!data_ || !data_->schema) return;
    data_->schema->pause(engine_.config()->deploy.migrationWait);
    data_->schema->waitForIdle();
}

void AppInstance::resume() {
    if (data_ && data_->schema) data_->schema->resume();
}

MigrateResult AppInstance::migrate(std::shared_ptr<const CompiledApp> app, const MigrationPlan& plan, bool seed) {
    MigrateResult r;
    std::vector<int> appliedBefore;
    if (auto applied = migrator_->applied()) {
        for (const auto& [number, record] : applied.value()) appliedBefore.push_back(number);
    }
    auto failed = [&](std::string message) {
        r.ok = false;
        r.applied.clear();
        r.message = std::move(message);
        lastMigrationError_ = r.message;
        resume();
        return r;
    };

    // Drain: hold new requests, and let those running finish.
    hold();
    // Backup: before anything changes.
    Status backedUp = fault("backup");
    if (backedUp && data_->backups) {
        auto b = data_->backups->backup("before-migrations", appliedBefore);
        if (b) {
            r.backup = b.value().name;
        } else {
            backedUp = b.error();
        }
    }
    if (!backedUp) {
        return failed("couldn't take a backup before migrating, so nothing was changed: " + backedUp.error().message);
    }

    // Migrate: the batch's database work is one transaction.
    migratingApp_ = app;
    Migrator::Outcome outcome = migrator_->apply(*app, plan.pending);
    migratingApp_.reset();
    if (!outcome.ok()) {
        std::string message = outcome.error->message;
        // The database is as it was (the transaction); the data files come
        // back from the backup.
        if (!r.backup.empty()) {
            Status restored = fault("restore");
            if (restored) restored = data_->backups->restore(r.backup, false, false);
            if (restored) {
                message += " (nothing was changed: the database work was undone, and the data files restored "
                           "from backup " + r.backup + ")";
            } else {
                message += "; restoring the data files from backup " + r.backup + " failed too (" +
                           restored.error().message + "): restore that backup with `fourfold restore`";
            }
        }
        return failed(message);
    }
    r.applied = outcome.applied;

    if (seed) {
        if (Status s = runSeed(app); !s) {
            r.message = "code/seed.ms failed: " + s.error().message;
        }
    }

    // Swap: the held requests go on to the new version.
    swap(app);
    if (data_->afterMigrations) data_->afterMigrations();
    resume();
    if (data_->backups) (void)data_->backups->prune(engine_.config()->backups.keep);

    lastMigrationError_.clear();
    r.ok = true;
    std::string applied = std::to_string(r.applied.size()) + (r.applied.size() == 1 ? " migration" : " migrations");
    if (r.message.empty()) {
        r.message = "Applied " + applied + (r.backup.empty() ? "" : " (backup first: " + r.backup + ")") + ".";
    } else {
        r.message = "Applied " + applied + ", but " + r.message;
    }
    return r;
}

namespace {

// Waits for a run's outcome, delivered by the script host's Done.
Status waitForRun(std::function<void(ScriptHost::Done)> start) {
    auto finished = std::make_shared<std::promise<HandlerOutcome>>();
    std::future<HandlerOutcome> result = finished->get_future();
    start([finished](HandlerOutcome o) { finished->set_value(std::move(o)); });
    HandlerOutcome o;
    try {
        o = result.get();
    } catch (const std::future_error&) {
        return makeError(ErrorCode::Internal, "the script host stopped");
    }
    if (o.kind != HandlerOutcome::Kind::Error) return {};
    const ScriptError& e = o.error;
    std::string where = e.file.empty() ? "" : e.file + (e.line > 0 ? ":" + std::to_string(e.line) : "") + ": ";
    return makeError(ErrorCode::Invalid, where + e.message);
}

}  // namespace

Status AppInstance::runMigrationScript(const Migration& m, DbConnection* tx) {
    if (!scripts_ || !migratingApp_) return makeError(ErrorCode::Internal, "no script host to run a migration");
    std::shared_ptr<const AppConfig> config = engine_.config();
    RunContext context;
    context.data = data_->files;
    context.db = tx;
    ScriptBudget budget = config->budget;
    budget.wallClock = config->deploy.migrationBudget;
    budget.instructions = 1'000'000'000'000ULL;   // bounded by its time instead
    context.budget = budget;
    std::shared_ptr<const CompiledApp> app = migratingApp_;
    int number = m.number;
    return waitForRun([&](ScriptHost::Done done) {
        scripts_->runMigration(app, number, std::move(context), std::move(done));
    });
}

Status AppInstance::runSeed(std::shared_ptr<const CompiledApp> app) {
    if (!scripts_) return makeError(ErrorCode::Internal, "no script host to run code/seed.ms");
    std::shared_ptr<const AppConfig> config = engine_.config();
    RunContext context;
    context.data = data_->files;
    context.db = app->usesDatabase() ? data_->db : nullptr;
    ScriptBudget budget = config->budget;
    budget.wallClock = config->deploy.migrationBudget;
    budget.instructions = 1'000'000'000'000ULL;
    context.budget = budget;
    return waitForRun([&](ScriptHost::Done done) { scripts_->runSeed(app, std::move(context), std::move(done)); });
}

template <typename T>
T AppInstance::onThread(std::function<T()> work) {
    std::promise<T> done;
    loop_->post([&] { done.set_value(work()); });
    return done.get_future().get();
}

MigrateResult AppInstance::migrateNow() {
    return onThread<MigrateResult>([this] {
        MigrateResult r;
        if (!migrator_) {
            r.message = "This app has no data to migrate.";
            return r;
        }
        std::shared_ptr<const CompiledApp> app;
        {
            std::lock_guard<std::mutex> lock(currentMutex_);
            app = waiting_;
        }
        if (!app) {
            if (!problems_.empty()) {
                r.message = "The newest code can't run on this data:\n  " + problems_[0];
                for (size_t i = 1; i < problems_.size(); i++) r.message += "\n  " + problems_[i];
                return r;
            }
            r.ok = true;
            r.message = newest_ ? "No migrations to apply." : "The app hasn't compiled; fix it first.";
            r.ok = newest_ != nullptr;
            return r;
        }
        auto applied = migrator_->applied();
        if (!applied) {
            r.message = "Can't read which migrations are applied: " + applied.error().message;
            return r;
        }
        MigrationPlan plan = planMigrations(*app, applied.value(), options_.allowOlderCode);
        if (!plan.ok()) {
            r.message = plan.errors[0];
            return r;
        }
        if (plan.pending.empty()) {
            swap(app);
            r.ok = true;
            r.message = "No migrations to apply.";
            return r;
        }
        r = migrate(app, plan, false);
        if (r.ok && observer_) {
            CompileReport report;
            report.ok = true;
            report.generation = app->generation;
            report.migrated = r.applied;
            report.backup = r.backup;
            observer_->compileFinished(report);
        }
        return r;
    });
}

size_t AppInstance::pendingMigrations() const {
    std::lock_guard<std::mutex> lock(currentMutex_);
    return waiting_ ? waitingCount_ : 0;
}

Result<BackupInfo> AppInstance::backupNow(const std::string& reason) {
    return onThread<Result<BackupInfo>>([this, reason]() -> Result<BackupInfo> {
        if (!data_ || !data_->backups) return makeError(ErrorCode::Invalid, "backups aren't set up");
        std::vector<int> applied;
        if (auto records = migrator_->applied()) {
            for (const auto& [number, record] : records.value()) applied.push_back(number);
        }
        hold();
        auto b = data_->backups->backup(reason, applied);
        resume();
        if (b && reason == "scheduled") (void)data_->backups->prune(engine_.config()->backups.keep);
        return b;
    });
}

MigrateResult AppInstance::redoLast() {
    return onThread<MigrateResult>([this] {
        MigrateResult r;
        if (!data_ || !data_->backups) {
            r.message = "Backups aren't set up, so there's nothing to go back to.";
            return r;
        }
        if (data_->db && data_->db->dialect() == SqlDialect::Postgres) {
            r.message = "Redo works with SQLite only: a Postgres database isn't in Fourfold's backups.";
            return r;
        }
        auto all = data_->backups->list();
        if (!all) {
            r.message = all.error().message;
            return r;
        }
        const BackupInfo* before = nullptr;
        for (const BackupInfo& b : all.value()) {
            if (b.reason == "before-migrations") {
                before = &b;
                break;
            }
        }
        if (!before) {
            r.message = "There's no backup from before the last migrations to go back to.";
            return r;
        }
        hold();
        if (data_->db) data_->db->closeIdle();
        Status s = data_->backups->restore(before->name, false, true);
        if (data_->db) data_->db->closeIdle();
        resume();
        if (!s) {
            r.message = "Restoring backup " + before->name + " failed: " + s.error().message;
            return r;
        }
        auto listed = snapshot();
        CompileReport report = compileNow(listed ? *listed : compiled_);
        if (observer_) observer_->compileFinished(report);
        r.ok = report.ok;
        r.applied = report.migrated;
        r.backup = report.backup;
        r.message = report.ok ? "Went back to " + before->name + ", and applied " +
                                    std::to_string(report.migrated.size()) + " migration(s) again."
                              : "Went back to " + before->name + ", but the app didn't compile or migrate; see the "
                                                                 "errors.";
        return r;
    });
}

MigrateResult AppInstance::resetData() {
    return onThread<MigrateResult>([this] {
        MigrateResult r;
        if (!data_) {
            r.message = "This app has no data to reset.";
            return r;
        }
        if (data_->db && data_->db->dialect() == SqlDialect::Postgres) {
            r.message = "Reset works with SQLite only; empty a Postgres database by hand.";
            return r;
        }
        hold();
        std::string backup;
        if (data_->backups) {
            auto b = data_->backups->backup("before-reset");
            if (!b) {
                resume();
                r.message = "Couldn't take a backup first, so nothing was changed: " + b.error().message;
                return r;
            }
            backup = b.value().name;
        }
        if (data_->db) data_->db->closeIdle();
        Status cleared = data_->data->clear([](const std::string& path, bool) { return path == ".fourfold/locks"; });
        (void)data_->data->databasePath();   // makes data/.fourfold/ again, for the database
        if (data_->db) data_->db->closeIdle();
        resume();
        if (!cleared) {
            r.message = "Emptying data/ failed: " + cleared.error().message +
                        (backup.empty() ? "" : " (backup " + backup + " has what was there)");
            return r;
        }
        auto listed = snapshot();
        CompileReport report = compileNow(listed ? *listed : compiled_);
        if (observer_) observer_->compileFinished(report);
        r.ok = report.ok;
        r.applied = report.migrated;
        r.backup = backup;
        r.message = report.ok ? "Reset: ran " + std::to_string(report.migrated.size()) + " migration(s)" +
                                    (report.seeded ? ", then code/seed.ms." : ".") +
                                    (backup.empty() ? "" : " The old data is in backup " + backup + ".")
                              : "Emptied the data, but the app didn't compile or migrate; see the errors.";
        return r;
    });
}

MigrationStatus AppInstance::migrationStatus() const {
    auto self = const_cast<AppInstance*>(this);
    return self->onThread<MigrationStatus>([this] {
        MigrationStatus st;
        st.lastError = lastMigrationError_;
        st.problems = problems_;
        if (!migrator_) return st;
        std::shared_ptr<const CompiledApp> app = newest_;
        {
            std::lock_guard<std::mutex> lock(currentMutex_);
            st.waiting = waiting_ != nullptr;
        }
        auto applied = migrator_->applied();
        AppliedMigrations records = applied ? applied.value() : AppliedMigrations{};
        if (!applied) st.lastError = "can't read which migrations are applied: " + applied.error().message;
        std::set<int> seen;
        if (app) {
            for (const Migration& m : app->migrations) {
                MigrationStatus::Item item;
                item.number = m.number;
                item.name = m.name;
                item.file = m.fileLabel();
                auto it = records.find(m.number);
                if (it != records.end()) {
                    item.state = it->second.checksum == m.checksum ? "applied" : "changed";
                    item.appliedAt = it->second.appliedAt;
                } else {
                    item.state = lastMigrationError_.rfind(m.name + ":", 0) == 0 ? "failed" : "pending";
                }
                seen.insert(m.number);
                st.items.push_back(std::move(item));
            }
        }
        for (const auto& [number, record] : records) {
            if (seen.count(number)) continue;
            MigrationStatus::Item item;
            item.number = number;
            item.name = record.name;
            item.file = record.file;
            item.state = "missing";
            item.appliedAt = record.appliedAt;
            st.items.push_back(std::move(item));
        }
        std::sort(st.items.begin(), st.items.end(),
                  [](const MigrationStatus::Item& a, const MigrationStatus::Item& b) { return a.number < b.number; });
        return st;
    });
}

}  // namespace fourfold

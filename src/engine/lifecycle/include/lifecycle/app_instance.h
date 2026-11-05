// AppInstance: one running app, recompiled as its code changes (Arch §5,
// lifecycle; overview §8 "Wait for quiet", §9), and migrated as its
// migrations change (overview §3, Applying migrations).
//
// It watches code/ by polling: every pollInterval it lists the tree and
// compares each file's size and modification time with the listing it last
// compiled. Once the tree has differed, and stayed the same for
// quietPeriod, it compiles a new version while the old one keeps serving,
// and swaps it into the engine. A compile that fails keeps the old version.
// Touching code/.deploy compiles at once, without waiting for quiet.
//
// Compiles run on the instance's own thread, one at a time. The .ms files
// are compiled on the script thread, a file at a time, between requests.
//
// It also watches private/config/ (M6), if given a ConfigWatch: a changed
// config is read again, and if it's valid, swapped into the engine; if not,
// the previous one stays (overview §10).
//
// Migrations (M8), if given the app's data services: a new version is
// checked against the migrations applied to the data. One that can't run
// on it (an applied migration changed or gone, or the data newer than the
// code) is refused like a failed compile. One with pending migrations is
// swapped in only after they're applied, by drain, backup, migrate, swap:
// requests are held at the schema gate, those running finish, a backup is
// taken, the migrations run (their database work in one transaction), and
// the new version goes in; if one fails, the data files are restored from
// the backup and the old version resumes. In dev mode that happens after
// each compile; otherwise the version waits for migrateNow() (`fourfold
// migrate`).
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "appdata/backup.h"
#include "appdata/data_files.h"
#include "appdata/database.h"
#include "appdata/migrator.h"
#include "base/result.h"
#include "compiledapp/compiled_app.h"
#include "config/app_config.h"
#include "observer/observer.h"
#include "partition/code_tree.h"
#include "pipeline/engine.h"
#include "pipeline/schema_gate.h"
#include "platform/event_loop.h"
#include "script/script_host.h"

namespace fourfold {

// Watching the config (M6).
struct ConfigWatch {
    ConfigSources sources;     // sources.dir must outlive the instance
    std::string stamp;         // configStamp() of the config already in use
    // Called on the instance's thread after each reload, valid or not (a
    // valid one is already in the engine).
    std::function<void(const ConfigLoad&)> loaded;
};

// The app's data, for migrations and backups (M8). Everything must outlive
// the instance.
struct DataServices {
    Database* db = nullptr;              // null: the app can't have a database
    const DataDir* data = nullptr;       // required
    DataFiles* files = nullptr;          // data/, for migrations' `data` and `file`
    BackupService* backups = nullptr;    // null: no backups (tests only)
    SchemaGate* schema = nullptr;        // null: requests aren't held (tests only)
    // Called after migrations ran, with the requests still held (e.g. to
    // clear anonymous sessions, if the config says so).
    std::function<void()> afterMigrations;
    // Whether the database exists yet (SQLite's file is made on first use,
    // so an app that never uses a database gets none). Null: it does.
    std::function<bool()> databaseExists;
};

struct LifecycleOptions {
    enum class Trigger {
        Quiet,        // compile once changes have stopped (dev); .deploy compiles at once
        DeployOnly,   // compile only when code/.deploy is touched
    };
    Trigger trigger = Trigger::Quiet;
    std::chrono::milliseconds pollInterval{250};
    // How long the tree must stay unchanged before compiling.
    std::chrono::milliseconds quietPeriod{200};
    // Reload private/config/ when it changes (needs a script host, which
    // evaluates it).
    std::optional<ConfigWatch> config;

    // Apply pending migrations after each compile (dev mode). Otherwise a
    // version with pending migrations waits for migrateNow().
    bool autoMigrate = true;
    // Run code/seed.ms after migrating a fresh database (dev mode).
    bool seed = true;
    // The emergency override: serve code older than the data's migrations.
    bool allowOlderCode = false;
    // For tests: fails a step ("backup", "before 3", "after 3", "commit",
    // "restore") as a real failure would.
    Migrator::FaultInjector faults;
};

// What migrating did (migrateNow, and dev mode's tools).
struct MigrateResult {
    bool ok = false;
    std::vector<int> applied;   // the migrations applied
    std::string backup;         // the backup taken first, if any
    std::string message;        // what happened, for the developer
};

// The state of the app's migrations, for the dev tools' panel.
struct MigrationStatus {
    struct Item {
        int number = 0;
        std::string name;           // "003_backfill"
        std::string file;           // "003_backfill.sql", "003_backfill/"
        std::string state;          // "applied", "pending", "failed", "changed", "missing"
        std::string appliedAt;      // if applied
    };
    std::vector<Item> items;        // by number
    std::string lastError;          // the last failure, if any
    std::vector<std::string> problems;   // why the newest version can't run on this data
    bool waiting = false;           // a version is waiting for its migrations
};

class AppInstance {
public:
    // `scripts` may be null for an app without code; `observer` and `data`
    // may be null. The engine, script host, observer and data services must
    // outlive the instance.
    static Result<std::unique_ptr<AppInstance>> create(CodeTree code, ScriptHost* scripts, Engine& engine,
                                                       Observer* observer, LifecycleOptions options = {},
                                                       DataServices* data = nullptr);
    // Stops watching, after any compile in progress.
    ~AppInstance();

    AppInstance(const AppInstance&) = delete;
    AppInstance& operator=(const AppInstance&) = delete;

    // The first compile, then watching. Waits for the compile, and returns
    // its report. Call once.
    CompileReport start();

    // Compiles now, whether or not anything changed, and waits for it.
    // Thread-safe (but not from an observer's compile events).
    CompileReport recompileNow();

    // The version serving now (the last good one), or null if none has
    // compiled yet.
    std::shared_ptr<const CompiledApp> current() const;

    // --- Migrations and backups (M8). Each waits; thread-safe. ---

    // Applies the waiting version's migrations: drain, backup, migrate,
    // swap. (What `fourfold migrate` asks a running server for.)
    MigrateResult migrateNow();
    // Pending migrations of the version waiting for them (0 if none is).
    size_t pendingMigrations() const;
    // A backup, with requests held while the files are copied.
    Result<BackupInfo> backupNow(const std::string& reason);
    // Dev mode: undoes the last batch of migrations (restoring the backup
    // taken before it) and applies them again, as they are now.
    MigrateResult redoLast();
    // Dev mode: empties data/ and the database, runs every migration, then
    // code/seed.ms.
    MigrateResult resetData();
    MigrationStatus migrationStatus() const;

private:
    // A file's identity for change detection.
    struct Stamp {
        uint64_t size = 0;
        int64_t modifiedNs = 0;
        bool operator==(const Stamp& o) const { return size == o.size && modifiedNs == o.modifiedNs; }
        bool operator!=(const Stamp& o) const { return !(*this == o); }
    };
    using Snapshot = std::map<RelPath, Stamp>;

    AppInstance(CodeTree code, ScriptHost* scripts, Engine& engine, Observer* observer, LifecycleOptions options,
                DataServices* data, std::unique_ptr<EventLoop> loop);

    // On the instance's thread.
    void schedulePoll();
    void poll();
    void pollConfig();
    CompileReport compileNow(const Snapshot& snapshot);
    std::optional<Snapshot> snapshot() const;
    // Puts a compiled version into service: checks it against the applied
    // migrations, then swaps it in, migrates first, or holds it. False if
    // it can't serve (with errors in the report).
    bool install(std::shared_ptr<const CompiledApp> app, CompileReport& report);
    void swap(std::shared_ptr<const CompiledApp> app);
    // Drain, backup, migrate, swap; then the seed, if `seed`.
    MigrateResult migrate(std::shared_ptr<const CompiledApp> app, const MigrationPlan& plan, bool seed);
    // Runs code/seed.ms of `app`, and waits.
    Status runSeed(std::shared_ptr<const CompiledApp> app);
    // Runs one .ms migration and waits (the Migrator's ScriptRunner).
    Status runMigrationScript(const Migration& m, DbConnection* tx);
    Status fault(const std::string& step) const;
    // Holds requests, waiting for those inside; resume() lets them go.
    void hold();
    void resume();
    // Runs `work` on the instance's thread, and waits for it.
    template <typename T>
    T onThread(std::function<T()> work);

    CodeTree code_;
    ScriptHost* scripts_;
    Engine& engine_;
    Observer* observer_;
    LifecycleOptions options_;
    DataServices* data_;
    std::unique_ptr<Migrator> migrator_;
    std::unique_ptr<EventLoop> loop_;
    std::thread thread_;

    // The instance's thread only.
    bool watching_ = false;
    bool compiledOnce_ = false;
    bool lastCompileOk_ = false;
    Snapshot compiled_;                       // as listed for the last compile
    std::optional<int64_t> deployTrigger_;    // as of the last compile
    std::optional<Snapshot> pending_;         // a changed tree, waiting for quiet
    EventLoop::Clock::time_point pendingSince_;
    uint64_t nextGeneration_ = 1;
    std::shared_ptr<const CompiledApp> migratingApp_;   // the version whose migrations are running
    std::shared_ptr<const CompiledApp> newest_;         // the newest version that compiled

    mutable std::mutex currentMutex_;
    std::shared_ptr<const CompiledApp> current_;
    // A version waiting for its migrations (production), and how many.
    std::shared_ptr<const CompiledApp> waiting_;
    size_t waitingCount_ = 0;
    std::string lastMigrationError_;
    std::vector<std::string> problems_;
};

}  // namespace fourfold

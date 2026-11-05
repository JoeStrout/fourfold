// Migrator: applies the app's migrations (overview §3, Schema changes and
// migrations; Arch §5 appdata), and keeps the record of what's applied
// with the data it describes:
//
//   - the database's in a fourfold_migrations table inside it;
//   - single-file .ms migrations' (which change data files only) in
//     data/.fourfold/migrations.json.
//
// So restoring a backup brings back the matching record. A batch of
// migrations does all its database work in one transaction, with the
// records: .sql migrations' statements, and everything folder migrations'
// migrate() does through db.query. If any migration fails, nothing of the
// batch stays in the database (both SQLite and Postgres can undo schema
// changes); undoing data file changes is the backup's job, which is the
// caller's (lifecycle).
//
// Blocking (database and file work, and waiting on the script thread for
// .ms migrations): run it on the lifecycle thread or a worker, never the
// script thread.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "appdata/database.h"
#include "base/clock.h"
#include "compiledapp/compiled_app.h"
#include "partition/data_dir.h"

namespace fourfold {

struct AppliedMigration {
    int number = 0;
    std::string name;        // "003_backfill_entry_time"
    std::string file;        // what was applied: "003_backfill_entry_time.pg.sql", "003_backfill_entry_time/"
    std::string checksum;
    std::string appliedAt;   // "2026-11-05T14:02:12Z"
    bool inDatabase = true;  // recorded in the database (else in data/.fourfold/)
};

using AppliedMigrations = std::map<int, AppliedMigration>;

// How the app's migrations compare with what's applied.
struct MigrationPlan {
    std::vector<const Migration*> pending;   // in order
    // Why this code can't run on this data, each a sentence naming the
    // migration: an applied one changed or gone, a new one numbered below
    // an applied one, or the data newer than the code (refused unless
    // allowed; then a warning).
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    bool ok() const { return errors.empty(); }
};

// `allowOlderCode`: the emergency override (overview §3, "Rolling back code
// past a migration is refused"): migrations applied that the code doesn't
// have are warnings, not errors.
MigrationPlan planMigrations(const CompiledApp& app, const AppliedMigrations& applied, bool allowOlderCode = false);

class Migrator {
public:
    // Runs a .ms migration of `app` (on the script thread) and waits for it.
    // `tx` is the migration's transaction (a folder migration), or null (a
    // single file, which has no database).
    using ScriptRunner = std::function<Status(const CompiledApp& app, const Migration& m, DbConnection* tx)>;
    // For tests: called at each step ("before 3", "after 3"); an error
    // fails the migration there, as a real failure would.
    using FaultInjector = std::function<Status(const std::string& step)>;

    // `db` may be null for an app without a database; `data` must outlive
    // the migrator.
    Migrator(Database* db, const DataDir& data, ScriptRunner scripts, const Clock& clock = systemClock());

    void setFaultInjector(FaultInjector inject) { inject_ = std::move(inject); }
    // Whether the database exists yet (a SQLite file is made on first use):
    // if not, nothing is applied to it, and reading the records doesn't
    // make one. Default: it always does.
    void setDatabaseExists(std::function<bool()> exists) { dbExists_ = std::move(exists); }

    // What's applied, from both records (the database's through `tx`, if
    // given).
    Result<AppliedMigrations> applied(DbConnection* tx = nullptr) const;

    struct Outcome {
        std::vector<int> applied;     // in order; empty if one failed
        std::optional<Error> error;   // the failure, if one failed
        int failed = 0;               // its number (0: not one migration's)

        bool ok() const { return !error; }
    };
    // Applies `pending` in order, stopping at the first failure, which
    // undoes the batch's database work. (Data files changed by .ms
    // migrations before the failure aren't undone here.) Steps for the
    // fault injector: "before N", "after N", "commit".
    Outcome apply(const CompiledApp& app, const std::vector<const Migration*>& pending);

    // For dev mode's "redo the last migration": forgets that `number` was
    // applied (its record only; the caller restores the data first).
    Status forget(int number);

private:
    Status applyOne(const CompiledApp& app, const Migration& m, DbConnection* tx);
    Status ensureTable() const;
    Status writeDataRecord(const AppliedMigrations& records) const;
    AppliedMigration recordFor(const Migration& m) const;
    Status inject(const std::string& step) const;

    Database* db_;
    const DataDir& data_;
    ScriptRunner scripts_;
    const Clock& clock_;
    FaultInjector inject_;
    std::function<bool()> dbExists_;
};

}  // namespace fourfold

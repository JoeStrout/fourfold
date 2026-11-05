// Migrator and BackupService (overview §3, Schema changes and migrations;
// Backups): plans, transactions, records kept with the data, failures
// leaving nothing behind, and backups restored exactly.
#include "appdata/migrator.h"

#include <fstream>
#include <sstream>

#include "appdata/backup.h"
#include "appdata/data_files.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "platform/sqlite.h"
#include "temp_dir.h"
#include "test_postgres.h"

using namespace fourfold;
using fourfold::test::TempDir;
using fourfold::test::TestPostgres;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

Migration sqlMigration(int number, const std::string& name, const std::string& sql) {
    Migration m;
    m.number = number;
    m.name = name;
    m.kind = Migration::Kind::Sql;
    m.source = RelPath::parse("migrations/" + name + ".sql").value();
    m.sql = sql;
    m.checksum = "sum-" + sql;
    return m;
}

Migration scriptMigration(int number, const std::string& name, bool folder) {
    Migration m;
    m.number = number;
    m.name = name;
    m.kind = folder ? Migration::Kind::Folder : Migration::Kind::Script;
    m.source = RelPath::parse("migrations/" + name + (folder ? "" : ".ms")).value();
    m.checksum = "sum-" + name;
    return m;
}

NamedQuery query(SqlDialect d, const std::string& sql) {
    std::vector<SqlProblem> problems;
    NamedQuery q;
    q.name = "q";
    q.sql = prepareSql(sql, d, problems);
    REQUIRE(problems.empty());
    return q;
}

double count(Database& db, const std::string& table) {
    auto rows = db.query(query(db.dialect(), "SELECT count(*) AS n FROM " + table), {});
    REQUIRE_MESSAGE(rows.ok(), rows.error().message);
    return rows.value().asList()[0].asMap().at("n").asNumber();
}

bool tableExists(Database& db, const std::string& table) {
    auto rows = db.query(query(db.dialect(), "SELECT count(*) AS n FROM " + table), {});
    return rows.ok();
}

struct App {
    TempDir dir;
    AppFolder folder = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    DataDir data = folder.dataDir().value();
    StateDir state = folder.stateDir().value();
};

// The migration tests both databases must pass.
void migrationTests(Database& db, const DataDir& data) {
    CAPTURE(sqlDialectName(db.dialect()));
    CompiledApp app;
    app.dialect = db.dialect();
    app.migrations.push_back(sqlMigration(1, "001_create", "CREATE TABLE entries (id INTEGER PRIMARY KEY, msg TEXT);"));
    app.migrations.push_back(sqlMigration(2, "002_more", "CREATE TABLE more (x INTEGER); INSERT INTO entries (id, msg) "
                                                         "VALUES (1, 'first');"));
    app.migrations.push_back(scriptMigration(3, "003_backfill", true));
    app.migrations.push_back(scriptMigration(4, "004_files", false));

    std::vector<std::string> ran;
    bool failScript = false;
    Migrator migrator(&db, data, [&](const CompiledApp&, const Migration& m, DbConnection* tx) -> Status {
        ran.push_back(m.name);
        if (m.kind == Migration::Kind::Folder) {
            REQUIRE(tx);
            auto r = tx->query(query(db.dialect(), "INSERT INTO entries (id, msg) VALUES (2, 'from script')"), {});
            if (!r) return r.error();
            if (failScript) return makeError(ErrorCode::Invalid, "migrate returned an error");
        } else {
            CHECK_FALSE(tx);
        }
        return {};
    });

    // Nothing applied yet: all pending.
    auto applied = migrator.applied();
    REQUIRE_MESSAGE(applied.ok(), applied.error().message);
    CHECK(applied.value().empty());
    MigrationPlan plan = planMigrations(app, applied.value());
    CHECK(plan.ok());
    REQUIRE_EQ(plan.pending.size(), 4u);

    // A folder migration that fails undoes the whole batch: its own insert,
    // and the migrations before it.
    failScript = true;
    Migrator::Outcome failed = migrator.apply(app, plan.pending);
    CHECK_FALSE(failed.ok());
    CHECK_EQ(failed.failed, 3);
    CHECK(contains(failed.error->message, "003_backfill: migrate returned an error"));
    CHECK(failed.applied.empty());
    CHECK_FALSE(tableExists(db, "entries"));
    CHECK(migrator.applied().value().empty());

    // Then it works.
    failScript = false;
    plan = planMigrations(app, migrator.applied().value());
    REQUIRE_EQ(plan.pending.size(), 4u);
    Migrator::Outcome ok = migrator.apply(app, plan.pending);
    REQUIRE_MESSAGE(ok.ok(), ok.error->message);
    CHECK_EQ(ok.applied, (std::vector<int>{1, 2, 3, 4}));
    CHECK_EQ(count(db, "entries"), 2);
    AppliedMigrations records = migrator.applied().value();
    REQUIRE_EQ(records.size(), 4u);
    CHECK(records.at(3).inDatabase);
    CHECK_EQ(records.at(3).file, "003_backfill/");
    CHECK_FALSE(records.at(4).inDatabase);   // data files only: recorded in data/.fourfold/
    CHECK_EQ(records.at(4).file, "004_files.ms");
    CHECK_EQ(records.at(1).checksum, app.migrations[0].checksum);
    CHECK(planMigrations(app, records).pending.empty());

    // A failing .sql migration: nothing of it stays, not even its first
    // statement.
    app.migrations.push_back(sqlMigration(5, "005_bad", "CREATE TABLE partial (x INTEGER); SELECT nope FROM nothing;"));
    plan = planMigrations(app, migrator.applied().value());
    Migrator::Outcome bad = migrator.apply(app, plan.pending);
    CHECK_FALSE(bad.ok());
    CHECK_FALSE(tableExists(db, "partial"));
    CHECK_FALSE(migrator.applied().value().count(5));

    // Failures injected at each step: rolled back every time.
    app.migrations.back() = sqlMigration(5, "005_good", "CREATE TABLE partial (x INTEGER);");
    app.migrations.push_back(sqlMigration(6, "006_more", "CREATE TABLE partial2 (x INTEGER);"));
    for (const char* at : {"before 5", "after 5", "before 6", "after 6", "commit"}) {
        CAPTURE(at);
        std::string step = at;
        migrator.setFaultInjector([step](const std::string& s) -> Status {
            if (s == step) return makeError(ErrorCode::IO, "injected");
            return {};
        });
        plan = planMigrations(app, migrator.applied().value());
        REQUIRE_EQ(plan.pending.size(), 2u);
        CHECK_FALSE(migrator.apply(app, plan.pending).ok());
        CHECK_FALSE(tableExists(db, "partial"));
        CHECK_FALSE(tableExists(db, "partial2"));
        CHECK_EQ(migrator.applied().value().size(), 4u);
    }
    migrator.setFaultInjector(nullptr);
    CHECK(migrator.apply(app, planMigrations(app, migrator.applied().value()).pending).ok());
    CHECK(tableExists(db, "partial"));

    // forget, for "redo the last migration".
    REQUIRE(migrator.forget(6).ok());
    REQUIRE(migrator.forget(5).ok());
    REQUIRE(migrator.forget(4).ok());
    CHECK_FALSE(migrator.applied().value().count(5));
    CHECK_FALSE(migrator.applied().value().count(4));
}

}  // namespace

TEST_CASE("migrations: planned against what's applied") {
    CompiledApp app;
    app.migrations.push_back(sqlMigration(1, "001_a", "A"));
    app.migrations.push_back(sqlMigration(2, "002_b", "B"));
    app.migrations.push_back(sqlMigration(5, "005_e", "E"));

    AppliedMigrations applied;
    auto record = [&](int n, const std::string& name, const std::string& sum) {
        AppliedMigration a;
        a.number = n;
        a.name = name;
        a.checksum = sum;
        a.appliedAt = "2026-11-01T00:00:00Z";
        applied[n] = a;
    };
    record(1, "001_a", "sum-A");
    MigrationPlan plan = planMigrations(app, applied);
    CHECK(plan.ok());
    REQUIRE_EQ(plan.pending.size(), 2u);
    CHECK_EQ(plan.pending[0]->number, 2);

    SUBCASE("an applied migration changed") {
        record(1, "001_a", "sum-old");
        MigrationPlan p = planMigrations(app, applied);
        REQUIRE_EQ(p.errors.size(), 1u);
        CHECK(contains(p.errors[0], "code/migrations/001_a.sql was changed after it was applied"));
    }
    SUBCASE("a new migration numbered below one applied") {
        record(5, "005_e", "sum-E");
        MigrationPlan p = planMigrations(app, applied);
        REQUIRE_EQ(p.errors.size(), 1u);
        CHECK(contains(p.errors[0], "002_b.sql is numbered below 005_e"));
    }
    SUBCASE("an applied migration gone from the code") {
        record(2, "002_b", "sum-B");
        record(3, "003_c", "sum-C");
        record(5, "005_e", "sum-E");
        MigrationPlan p = planMigrations(app, applied);
        REQUIRE_EQ(p.errors.size(), 1u);
        CHECK(contains(p.errors[0], "003_c was applied"));
        CHECK(contains(p.errors[0], "no longer has it"));
    }
    SUBCASE("older code than the data: refused, unless overridden") {
        record(2, "002_b", "sum-B");
        record(5, "005_e", "sum-E");
        record(7, "007_g", "sum-G");
        MigrationPlan p = planMigrations(app, applied);
        REQUIRE_EQ(p.errors.size(), 1u);
        CHECK(contains(p.errors[0], "007_g was applied to this data, but this code doesn't have it"));
        MigrationPlan forced = planMigrations(app, applied, true);
        CHECK(forced.ok());
        CHECK_EQ(forced.warnings.size(), 1u);
    }
}

TEST_CASE("migrations: applied on SQLite") {
    App a;
    auto db = Database::openSqlite(a.data.databasePath().value());
    REQUIRE(db.ok());
    migrationTests(*db.value(), a.data);
}

TEST_CASE("migrations: applied on Postgres") {
    if (!TestPostgres::settings()) {
        MESSAGE("skipped: set FOURFOLD_TEST_PG_URL to run the Postgres tests");
        return;
    }
    TestPostgres pg;
    REQUIRE_MESSAGE(pg.ok(), pg.error());
    App a;
    auto db = Database::openPostgres(pg.config());
    REQUIRE(db.ok());
    migrationTests(*db.value(), a.data);
    db.value()->closeIdle();
}

TEST_CASE("backups: taken and restored exactly") {
    App a;
    DataFiles files(a.folder.dataDir().value());
    REQUIRE(files.set(DataFiles::checkName("guestbook").value(), Datum::string("v1")).ok());
    REQUIRE(a.data.write(RelPath::parse("photos/cat.png").value(), "png-bytes").ok());
    REQUIRE(a.state.write(RelPath::parse("engine.key").value(), "secret").ok());
    REQUIRE(a.state.append(RelPath::parse("logs/engine.log").value(), "a log line\n").ok());
    std::string dbPath = a.data.databasePath().value();
    {
        auto db = Sqlite::open(dbPath).value();
        REQUIRE(db->exec("CREATE TABLE t (x); INSERT INTO t VALUES (1);").ok());
        auto state = Sqlite::open(a.state.sqlitePath(RelPath::parse("state.db").value()).value()).value();
        REQUIRE(state->exec("CREATE TABLE sessions (id); INSERT INTO sessions VALUES ('s1');").ok());
    }

    BackupService backups(a.data, a.state, a.state.folderPath(RelPath::parse("backups").value()).value(), true);
    auto taken = backups.backup("before-migrations", {1, 2});
    REQUIRE_MESSAGE(taken.ok(), taken.error().message);
    CHECK(contains(taken.value().name, "-before-migrations"));

    // Change everything.
    REQUIRE(files.set(DataFiles::checkName("guestbook").value(), Datum::string("v2")).ok());
    REQUIRE(a.data.remove(RelPath::parse("photos/cat.png").value()).ok());
    REQUIRE(a.data.write(RelPath::parse("new.txt").value(), "added later").ok());
    {
        auto db = Sqlite::open(dbPath).value();
        REQUIRE(db->exec("DROP TABLE t; CREATE TABLE other (y);").ok());
    }

    // Data only (as after a failed migration): back as it was, the extra
    // file gone; state untouched.
    REQUIRE(backups.restore(taken.value().name, false).ok());
    CHECK_EQ(files.get(DataFiles::checkName("guestbook").value()).value().value(), Datum::string("v1"));
    CHECK_EQ(a.data.read(RelPath::parse("photos/cat.png").value(), 100).value(), "png-bytes");
    CHECK_FALSE(a.data.exists(RelPath::parse("new.txt").value()).value());
    {
        auto db = Sqlite::open(dbPath).value();
        auto st = db->statement("SELECT x FROM t");
        REQUIRE_MESSAGE(st.ok(), st.error().message);
        REQUIRE(st.value()->step().value());
        CHECK_EQ(st.value()->integer(0), 1);
    }

    // With state.
    REQUIRE(a.state.write(RelPath::parse("engine.key").value(), "changed").ok());
    REQUIRE(backups.restore(taken.value().name, true).ok());
    CHECK_EQ(a.state.read(RelPath::parse("engine.key").value(), 100).value(), "secret");
    CHECK(a.state.exists(RelPath::parse("logs/engine.log").value()).value());   // logs aren't restored over

    // Listing and pruning: automatic backups beyond the newest kept;
    // others stay.
    ManualClock clock(1'800'000'000'000);
    BackupService timed(a.data, a.state, backups.folder(), true, clock);
    for (int i = 0; i < 3; i++) {
        clock.advanceSeconds(1);
        REQUIRE(timed.backup("scheduled").ok());
    }
    clock.advanceSeconds(1);
    REQUIRE(timed.backup("manual").ok());
    auto listed = timed.list();
    REQUIRE(listed.ok());
    CHECK_EQ(listed.value().size(), 5u);
    CHECK_EQ(listed.value()[0].reason, "manual");   // newest first
    REQUIRE(timed.prune(2).ok());
    listed = timed.list();
    CHECK_EQ(listed.value().size(), 3u);   // the manual one, and the newest two automatic ones

    CHECK_FALSE(backups.restore("../etc", true).ok());
    CHECK_FALSE(backups.restore("nonesuch", true).ok());
}

// The database pass (overview §3, SQL dialects; Schema changes and
// migrations): named queries and their variants, migrations and their
// rules, and the seed.
#include "compiler/compiler.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

CompileOutput compileApp(const std::string& root, SqlDialect dialect = SqlDialect::Sqlite) {
    auto app = AppFolder::open(root, AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto tree = app.value().codeTree();
    REQUIRE(tree.ok());
    auto host = ScriptHost::create();
    REQUIRE(host.ok());
    CompileOptions options;
    options.scripts = host.value().get();
    options.evaluator = host.value().get();
    options.dialect = dialect;
    return compile(tree.value(), options);
}

std::string allDiagnostics(const CompileOutput& out) {
    std::string s;
    for (const Diagnostic& d : out.diagnostics) s += d.toString() + "\n";
    return s;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

const Migration* migration(const CompiledApp& app, int number) {
    for (const Migration& m : app.migrations) {
        if (m.number == number) return &m;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("database: queries, with variants chosen for the app's database") {
    TempDir dir;
    dir.write("code/queries/findUser.sql", "SELECT * FROM users WHERE email = {{0}}");
    dir.write("code/queries/recent.sqlite.sql", "SELECT * FROM posts WHERE t > datetime('now', {{age}})");
    dir.write("code/queries/recent.pg.sql", "SELECT * FROM posts WHERE t > now() - {{age}}::interval");
    dir.write("code/queries/admin/users.sql", "SELECT * FROM users");
    dir.write("code/queries/notes.txt", "not a query");

    CompileOutput sqlite = compileApp(dir.path());
    REQUIRE_MESSAGE(sqlite.ok(), allDiagnostics(sqlite));
    CHECK(contains(allDiagnostics(sqlite), "code/queries/notes.txt: warning: only .sql files"));
    REQUIRE_EQ(sqlite.app->queries.size(), 3u);
    CHECK_EQ(sqlite.app->queries.at("recent")->source.str(), "queries/recent.sqlite.sql");
    CHECK_EQ(sqlite.app->queries.at("findUser")->sql.sql, "SELECT * FROM users WHERE email = ?1");
    CHECK(sqlite.app->queries.count("admin/users"));
    CHECK(sqlite.app->usesDatabase());
    CHECK(contains(describe(*sqlite.app), "queries (SQLite):\n  admin/users  queries/admin/users.sql\n"));

    CompileOutput pg = compileApp(dir.path(), SqlDialect::Postgres);
    REQUIRE_MESSAGE(pg.ok(), allDiagnostics(pg));
    CHECK_EQ(pg.app->queries.at("recent")->source.str(), "queries/recent.pg.sql");
    CHECK_EQ(pg.app->queries.at("findUser")->sql.sql, "SELECT * FROM users WHERE email = $1");
    CHECK_EQ(pg.app->dialect, SqlDialect::Postgres);
}

TEST_CASE("database: mistakes in queries") {
    SUBCASE("a variant for the other database only") {
        TempDir dir;
        dir.write("code/queries/x.pg.sql", "SELECT 1");
        CompileOutput out = compileApp(dir.path());
        CHECK_FALSE(out.ok());
        CHECK(contains(allDiagnostics(out), "query x has no file for SQLite"));
        // Ignored, and fine, on Postgres.
        CHECK(compileApp(dir.path(), SqlDialect::Postgres).ok());
    }
    SUBCASE("variants that take different values") {
        TempDir dir;
        dir.write("code/queries/x.sql", "SELECT * FROM t WHERE a = {{a}} AND b = {{b}}");
        dir.write("code/queries/x.pg.sql", "SELECT * FROM t WHERE a = {{a}}");
        CompileOutput out = compileApp(dir.path());
        CHECK_FALSE(out.ok());
        CHECK(contains(allDiagnostics(out), "code/queries/x.pg.sql: error: uses {{a}}, but x.sql uses {{a}}, {{b}}"));
    }
    SUBCASE("variants with the same keys in another order are fine") {
        TempDir dir;
        dir.write("code/queries/x.sql", "INSERT INTO t (a, b) VALUES ({{0}}, {{1}})");
        dir.write("code/queries/x.pg.sql", "INSERT INTO t (b, a) VALUES ({{1}}, {{0}})");
        CHECK(compileApp(dir.path()).ok());
    }
    SUBCASE("two statements, none, a bad name, and a bad placeholder") {
        TempDir dir;
        dir.write("code/queries/two.sql", "SELECT 1; SELECT 2;");
        dir.write("code/queries/none.sql", "-- nothing yet");
        dir.write("code/queries/bad-name.sql", "SELECT 1");
        dir.write("code/queries/quoted.sql", "SELECT * FROM t WHERE name = '{{name}}'");
        std::string d = allDiagnostics(compileApp(dir.path()));
        CHECK(contains(d, "code/queries/two.sql: error: holds 2 statements"));
        CHECK(contains(d, "code/queries/none.sql: error: is empty"));
        CHECK(contains(d, "code/queries/bad-name.sql: error: a query's name"));
        CHECK(contains(d, "code/queries/quoted.sql:1: error: placeholders can't go inside quotes"));
    }
}

TEST_CASE("database: migrations of each kind") {
    TempDir dir;
    dir.write("code/migrations/001_create.sqlite.sql", "CREATE TABLE entries (id INTEGER PRIMARY KEY, msg TEXT);");
    dir.write("code/migrations/001_create.pg.sql", "CREATE TABLE entries (id SERIAL PRIMARY KEY, msg TEXT);");
    dir.write("code/migrations/002_add_time.sql", "ALTER TABLE entries ADD COLUMN t TEXT;");
    dir.write("code/migrations/003_backfill/migrate.ms",
              "import \"util\"\n"
              "migrate = function\n"
              "    for row in db.query(\"missing\")\n"
              "        db.query(\"setTime\", {\"id\": row.id, \"t\": util.now})\n"
              "    end for\n"
              "end function\n");
    dir.write("code/migrations/003_backfill/util.ms", "now = \"2026-11-05\"\n");
    dir.write("code/migrations/003_backfill/missing.sql", "SELECT id FROM entries WHERE t IS NULL");
    dir.write("code/migrations/003_backfill/setTime.sql", "UPDATE entries SET t = {{t}} WHERE id = {{id}}");
    dir.write("code/migrations/10_split.ms", "migrate = function\n    data.set(\"x\", 1)\nend function\n");
    dir.write("code/seed.ms", "print \"seeding\"\n");

    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const CompiledApp& app = *out.app;
    REQUIRE_EQ(app.migrations.size(), 4u);
    CHECK_EQ(app.migrations[0].number, 1);
    CHECK_EQ(app.migrations[3].number, 10);   // by number, not by name

    const Migration* create = migration(app, 1);
    CHECK(create->kind == Migration::Kind::Sql);
    CHECK_EQ(create->source.str(), "migrations/001_create.sqlite.sql");
    CHECK(contains(create->sql, "INTEGER PRIMARY KEY"));
    CHECK_EQ(create->checksum.size(), 64u);

    const Migration* backfill = migration(app, 3);
    CHECK(backfill->kind == Migration::Kind::Folder);
    CHECK_EQ(backfill->fileLabel(), "003_backfill/");
    CHECK_EQ(backfill->queries.size(), 2u);
    CHECK(backfill->queries.count("setTime"));
    CHECK(app.queries.empty());   // a migration's queries aren't the app's

    const Migration* split = migration(app, 10);
    CHECK(split->kind == Migration::Kind::Script);
    CHECK_FALSE(split->usesDatabase());
    REQUIRE(app.seed);
    CHECK_EQ(app.seed->str(), "seed.ms");
    CHECK(contains(describe(app), "migrations:\n  1  001_create.sqlite.sql\n"));

    // Each database gets its own variant, so its own checksum.
    CompileOutput pg = compileApp(dir.path(), SqlDialect::Postgres);
    REQUIRE(pg.ok());
    CHECK(migration(*pg.app, 1)->checksum != create->checksum);
    CHECK_EQ(migration(*pg.app, 2)->checksum, migration(app, 2)->checksum);
}

TEST_CASE("database: checksums follow what's applied") {
    TempDir dir;
    dir.write("code/migrations/001_a.sql", "CREATE TABLE a (x);");
    dir.write("code/migrations/001_a.pg.sql", "CREATE TABLE a (x int);");
    dir.write("code/migrations/002_b/migrate.ms", "migrate = function\nend function\n");
    dir.write("code/migrations/002_b/q.sql", "SELECT 1");
    CompileOutput before = compileApp(dir.path());
    REQUIRE(before.ok());

    // Editing the other database's variant changes nothing here.
    dir.write("code/migrations/001_a.pg.sql", "CREATE TABLE a (x bigint);");
    CompileOutput same = compileApp(dir.path());
    CHECK_EQ(migration(*same.app, 1)->checksum, migration(*before.app, 1)->checksum);

    // Adding a variant for this database does.
    dir.write("code/migrations/001_a.sqlite.sql", "CREATE TABLE a (x INTEGER);");
    CHECK(migration(*compileApp(dir.path()).app, 1)->checksum != migration(*before.app, 1)->checksum);

    // So does any file in a folder migration.
    dir.write("code/migrations/002_b/q2.sql", "SELECT 2");
    CHECK(migration(*compileApp(dir.path()).app, 2)->checksum != migration(*before.app, 2)->checksum);
}

TEST_CASE("database: mistakes in migrations") {
    SUBCASE("two migrations with one number") {
        TempDir dir;
        dir.write("code/migrations/003_a.sql", "CREATE TABLE a (x);");
        dir.write("code/migrations/3_b.ms", "migrate = function\nend function\n");
        CHECK(contains(allDiagnostics(compileApp(dir.path())), "two migrations are numbered 3: 003_a and 3_b"));
    }
    SUBCASE("names that aren't migrations") {
        TempDir dir;
        dir.write("code/migrations/create.sql", "CREATE TABLE a (x);");
        dir.write("code/migrations/001_a.txt", "notes");
        std::string d = allDiagnostics(compileApp(dir.path()));
        CHECK(contains(d, "code/migrations/create.sql: error: isn't a migration"));
        CHECK(contains(d, "code/migrations/001_a.txt: error: isn't a migration"));
    }
    SUBCASE("a .sql migration with a placeholder, and an empty one") {
        TempDir dir;
        dir.write("code/migrations/001_a.sql", "INSERT INTO t VALUES ({{x}});");
        dir.write("code/migrations/002_b.sql", "-- todo");
        std::string d = allDiagnostics(compileApp(dir.path()));
        CHECK(contains(d, "code/migrations/001_a.sql: error: a .sql migration runs as it is"));
        CHECK(contains(d, "code/migrations/002_b.sql: error: is empty"));
    }
    SUBCASE("a folder without migrate.ms, or with something else in it") {
        TempDir dir;
        dir.write("code/migrations/001_a/helper.ms", "x = 1\n");
        dir.write("code/migrations/001_a/notes.md", "hi");
        std::string d = allDiagnostics(compileApp(dir.path()));
        CHECK(contains(d, "code/migrations/001_a/: error: a migration's folder needs a migrate.ms"));
        CHECK(contains(d, "code/migrations/001_a/notes.md: error: doesn't belong in a migration's folder"));
    }
    SUBCASE("a single-file migration that uses db") {
        TempDir dir;
        dir.write("code/migrations/001_a.ms", "migrate = function\n    db.query(\"x\")\nend function\n");
        CHECK(contains(allDiagnostics(compileApp(dir.path())),
                       "code/migrations/001_a.ms: error: uses db: a migration that uses the database must be a folder"));
    }
    SUBCASE("a migration that imports app code") {
        TempDir dir;
        dir.write("code/lib/util.ms", "x = 1\n");
        dir.write("code/migrations/001_a/migrate.ms", "import \"util\"\nmigrate = function\nend function\n");
        dir.write("code/migrations/002_b.ms", "import \"util\"\nmigrate = function\nend function\n");
        std::string d = allDiagnostics(compileApp(dir.path()));
        CHECK(contains(d, "code/migrations/001_a/migrate.ms: error: imports \"util\", which isn't in its folder"));
        CHECK(contains(d, "code/migrations/002_b.ms: error: imports \"util\": a single-file migration can't import"));
    }
    SUBCASE("a migration that doesn't compile") {
        TempDir dir;
        dir.write("code/migrations/001_a.ms", "migrate = function\n    if\nend function\n");
        CHECK(contains(allDiagnostics(compileApp(dir.path())), "code/migrations/001_a.ms:"));
    }
}

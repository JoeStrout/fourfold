// Database (overview §3): the same query tests, on SQLite and on Postgres
// (when FOURFOLD_TEST_PG_URL names a server; see test_postgres.h).
#include "appdata/database.h"

#include <atomic>
#include <thread>

#include "doctest.h"
#include "temp_dir.h"
#include "test_postgres.h"

using namespace fourfold;
using fourfold::test::TempDir;
using fourfold::test::TestPostgres;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A named query, prepared for `db`'s dialect; `pg` replaces the SQL there.
NamedQuery q(const Database& db, const std::string& sql, const std::string& pg = "") {
    std::vector<SqlProblem> problems;
    NamedQuery out;
    out.name = "test";
    out.sql = prepareSql(db.dialect() == SqlDialect::Postgres && !pg.empty() ? pg : sql, db.dialect(), problems);
    REQUIRE_MESSAGE(problems.empty(), problems[0].message);
    return out;
}

Datum run(Database& db, const NamedQuery& query, Datum values = Datum()) {
    auto params = bindValues(query.sql, values);
    REQUIRE_MESSAGE(params.ok(), params.error().message);
    auto rows = db.query(query, params.value());
    REQUIRE_MESSAGE(rows.ok(), rows.error().message);
    return rows.value();
}

Error fails(Database& db, const NamedQuery& query, Datum values = Datum()) {
    auto params = bindValues(query.sql, values);
    REQUIRE(params.ok());
    auto rows = db.query(query, params.value());
    REQUIRE_FALSE(rows.ok());
    return rows.error();
}

Datum::ListItems list(std::initializer_list<Datum> items) { return Datum::ListItems(items); }
Datum str(const char* s) { return Datum::string(s); }
Datum num(double x) { return Datum::number(x); }

// The tests both databases must pass.
void sharedQueryTests(Database& db) {
    SqlDialect d = db.dialect();
    CAPTURE(sqlDialectName(d));
    std::unique_ptr<DbConnection> setup = db.connection().value();
    REQUIRE(setup->exec(d == SqlDialect::Sqlite
                            ? "CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, name TEXT, "
                              "score REAL, prefs TEXT);"
                            : "CREATE TABLE users (id SERIAL PRIMARY KEY, email TEXT UNIQUE NOT NULL, name TEXT, "
                              "score DOUBLE PRECISION, prefs JSONB, active BOOLEAN DEFAULT true);"));
    setup.reset();

    // Insert with RETURNING, values from a map.
    NamedQuery add = q(db, "INSERT INTO users (email, name, score, prefs) VALUES ({{email}}, {{name}}, {{score}}, "
                           "{{prefs}}) RETURNING id",
                       "INSERT INTO users (email, name, score, prefs) VALUES ({{email}}, {{name}}, {{score}}, "
                       "{{prefs}}::jsonb) RETURNING id");
    Datum r1 = run(db, add, Datum::map({{"email", str("a@x.org")}, {"name", str("Ann")}, {"score", num(2.5)},
                                        {"prefs", Datum::map({{"dark", num(1)}})}}));
    REQUIRE_EQ(r1.asList().size(), 1u);
    CHECK_EQ(r1.asList()[0].asMap().at("id"), num(1));
    run(db, add, Datum::map({{"email", str("b@x.org")}, {"name", Datum()}, {"score", num(7)}, {"prefs", Datum()}}));

    // Values from a list; types out.
    NamedQuery find = q(db, "SELECT id, email, name, score FROM users WHERE email = {{0}}");
    Datum rows = run(db, find, Datum::list(list({str("b@x.org")})));
    REQUIRE_EQ(rows.asList().size(), 1u);
    const Datum::MapItems& b = rows.asList()[0].asMap();
    CHECK_EQ(b.at("id"), num(2));
    CHECK_EQ(b.at("email"), str("b@x.org"));
    CHECK(b.at("name").isNull());
    CHECK_EQ(b.at("score"), num(7));

    // Nothing found: an empty list. A statement without rows: likewise.
    CHECK(run(db, find, Datum::list(list({str("nobody")}))).asList().empty());
    NamedQuery rename = q(db, "UPDATE users SET name = {{name}} WHERE id = {{id}}");
    CHECK(run(db, rename, Datum::map({{"name", str("Bea")}, {"id", num(2)}})).asList().empty());
    CHECK_EQ(run(db, find, Datum::list(list({str("b@x.org")}))).asList()[0].asMap().at("name"), str("Bea"));

    // Text is UTF-8, both ways; a value can't break out of its parameter.
    std::string tricky = "Zoë'); DROP TABLE users; --";
    run(db, rename, Datum::map({{"name", Datum::string(tricky)}, {"id", num(1)}}));
    CHECK_EQ(run(db, find, Datum::list(list({str("a@x.org")}))).asList()[0].asMap().at("name"), Datum::string(tricky));

    // A list or map value is stored as JSON.
    NamedQuery prefs = q(db, "SELECT prefs FROM users WHERE id = 1");
    Datum p = run(db, prefs).asList()[0].asMap().at("prefs");
    if (d == SqlDialect::Postgres) {
        CHECK_EQ(p, Datum::map({{"dark", num(1)}}));   // jsonb comes back parsed
    } else {
        CHECK_EQ(p, str("{\"dark\":1}"));
    }

    // Numbers: whole numbers are integers; fractions stay fractions.
    NamedQuery math = q(db, "SELECT {{0}} + 1 AS n, {{1}} * 2 AS f", "SELECT {{0}}::int + 1 AS n, {{1}}::float * 2 AS f");
    Datum m = run(db, math, Datum::list(list({num(41), num(0.25)}))).asList()[0];
    CHECK_EQ(m.asMap().at("n"), num(42));
    CHECK_EQ(m.asMap().at("f"), num(0.5));

    // Errors: a constraint, bad SQL at run time.
    Error dup = fails(db, add, Datum::map({{"email", str("a@x.org")}, {"name", str("again")}, {"score", num(0)},
                                           {"prefs", Datum()}}));
    CHECK(dup.code == ErrorCode::Conflict);
    CHECK((contains(dup.message, "UNIQUE") || contains(dup.message, "unique")));
    Error missing = fails(db, q(db, "SELECT nope FROM users"));
    CHECK(contains(missing.message, "nope"));

    // Binary data can't come back.
    Error blob = fails(db, q(db, "SELECT X'00ff' AS b", "SELECT '\\x00ff'::bytea AS b"));
    CHECK(contains(blob.message, "binary data"));

    // A transaction: rolled back, then committed.
    {
        auto conn = db.connection().value();
        REQUIRE(conn->begin().ok());
        auto params = bindValues(add.sql, Datum::map({{"email", str("c@x.org")}, {"name", str("C")},
                                                       {"score", num(1)}, {"prefs", Datum()}}));
        REQUIRE(conn->query(add, params.value()).ok());
        REQUIRE(conn->rollback().ok());
    }
    CHECK(run(db, find, Datum::list(list({str("c@x.org")}))).asList().empty());
    {
        auto conn = db.connection().value();
        REQUIRE(conn->begin().ok());
        auto params = bindValues(add.sql, Datum::map({{"email", str("c@x.org")}, {"name", str("C")},
                                                       {"score", num(1)}, {"prefs", Datum()}}));
        REQUIRE(conn->query(add, params.value()).ok());
        REQUIRE(conn->commit().ok());
    }
    CHECK_EQ(run(db, find, Datum::list(list({str("c@x.org")}))).asList().size(), 1u);

    // A connection let go mid-transaction rolls back.
    {
        auto conn = db.connection().value();
        REQUIRE(conn->begin().ok());
        REQUIRE(conn->exec("DELETE FROM users").ok());
    }
    CHECK_EQ(run(db, q(db, "SELECT count(*) AS n FROM users")).asList()[0].asMap().at("n"), num(3));

    // Several threads at once.
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 10; i++) {
                std::string email = "t" + std::to_string(t) + "-" + std::to_string(i) + "@x.org";
                auto params = bindValues(add.sql, Datum::map({{"email", Datum::string(email)}, {"name", Datum()},
                                                               {"score", num(i)}, {"prefs", Datum()}}));
                if (!db.query(add, params.value())) failures++;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(run(db, q(db, "SELECT count(*) AS n FROM users")).asList()[0].asMap().at("n"), num(83));
}

void limitTests(Database& db) {
    CAPTURE(sqlDialectName(db.dialect()));
    // Too many rows.
    NamedQuery many = q(db, "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 50) SELECT i FROM n");
    Error tooMany = fails(db, many);
    CHECK(tooMany.code == ErrorCode::TooLarge);
    CHECK(contains(tooMany.message, "more than 20 rows"));
    // Too long.
    NamedQuery slow = q(db, "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 100000000) "
                            "SELECT count(*) AS c FROM n",
                        "SELECT pg_sleep(2)");
    auto t0 = std::chrono::steady_clock::now();
    Error tooLong = fails(db, slow);
    CHECK(contains(tooLong.message, "took longer than its time limit"));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    // The pool still works afterward.
    CHECK_EQ(run(db, q(db, "SELECT 1 AS one")).asList()[0].asMap().at("one"), num(1));
}

DbLimits smallLimits() {
    DbLimits limits;
    limits.maxRows = 20;
    limits.queryTimeout = std::chrono::milliseconds(200);
    return limits;
}

}  // namespace

TEST_CASE("database: SQLite passes the shared query tests") {
    TempDir dir;
    auto db = Database::openSqlite(dir.at("app.sqlite"));
    REQUIRE(db.ok());
    sharedQueryTests(*db.value());
}

TEST_CASE("database: SQLite limits") {
    TempDir dir;
    auto db = Database::openSqlite(dir.at("app.sqlite"), smallLimits());
    REQUIRE(db.ok());
    limitTests(*db.value());
}

TEST_CASE("database: Postgres passes the shared query tests") {
    TestPostgres pg;
    if (!TestPostgres::settings()) {
        MESSAGE("skipped: set FOURFOLD_TEST_PG_URL to run the Postgres tests");
        return;
    }
    REQUIRE_MESSAGE(pg.ok(), pg.error());
    auto db = Database::openPostgres(pg.config());
    REQUIRE_MESSAGE(db.ok(), db.error().message);
    sharedQueryTests(*db.value());
    db.value()->closeIdle();
}

TEST_CASE("database: Postgres limits") {
    TestPostgres pg;
    if (!TestPostgres::settings()) return;
    REQUIRE_MESSAGE(pg.ok(), pg.error());
    auto db = Database::openPostgres(pg.config(), smallLimits());
    REQUIRE_MESSAGE(db.ok(), db.error().message);
    limitTests(*db.value());
    db.value()->closeIdle();
}

TEST_CASE("database: Postgres refused cleanly when it can't connect") {
    PostgresSettings bad;
    bad.url = "postgres://127.0.0.1:1/nothing";
    auto db = Database::openPostgres(bad);
    REQUIRE_FALSE(db.ok());
    if (Database::postgresSupported()) {
        CHECK(contains(db.error().message, "can't connect to Postgres"));
    } else {
        CHECK(contains(db.error().message, "no Postgres support"));
    }
}

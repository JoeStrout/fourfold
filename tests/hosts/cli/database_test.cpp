// End to end (M8): databases and migrations, through the real server and
// CLI.
//
// The exit criteria (milestones.md, M8): a migration that fails partway
// leaves the database and data files exactly as they were (tested by
// injecting failures at each step); a long migration under load holds
// requests and then serves them on the new version; Postgres and SQLite
// pass the same query tests (tests/appdata/database_test.cpp, and the
// failure test here on both).
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "app_server.h"
#include "browser.h"
#include "cli.h"
#include "doctest.h"
#include "platform/sqlite.h"
#include "temp_dir.h"
#include "test_postgres.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::cli::ServerOptions;
using fourfold::test::rawFetch;
using fourfold::test::TempDir;
using fourfold::test::TestPostgres;

namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int statusOf(const std::string& response) { return response.size() > 12 ? std::atoi(response.c_str() + 9) : 0; }

std::string bodyText(const std::string& response) {
    size_t at = response.find("\r\n\r\n");
    return at == std::string::npos ? "" : response.substr(at + 4);
}

std::string get(uint16_t port, const std::string& target) {
    return rawFetch(port, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
}

struct Running {
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;

    Running(const std::string& root, ServerOptions options) {
        server = AppServer::start(root, std::move(options), out, err);
        REQUIRE_MESSAGE(server, err.str());
        serving = std::thread([this] { server->loop().run(); });
    }
    ~Running() {
        server->loop().stop();
        serving.join();
    }
    uint16_t port() const { return server->port(); }
};

ServerOptions devOptions() {
    ServerOptions o;
    o.dev = true;
    return o;
}

ServerOptions serveOptions() {
    ServerOptions o;
    o.dev = false;
    o.https = false;
    return o;
}

ServerOptions offlineOptions() {
    ServerOptions o;
    o.dev = false;
    o.listen = false;
    return o;
}

// The notes app: a table, its queries, and pages that use them.
void writeNotesApp(const TempDir& dir) {
    dir.write("app/code/migrations/001_create.sql",
              "CREATE TABLE notes (id INTEGER PRIMARY KEY, text TEXT NOT NULL);\n"
              "INSERT INTO notes (id, text) VALUES (1, 'first');\n");
    dir.write("app/code/migrations/001_create.pg.sql",
              "CREATE TABLE notes (id SERIAL PRIMARY KEY, text TEXT NOT NULL);\n"
              "INSERT INTO notes (text) VALUES ('first');\n");
    dir.write("app/code/queries/addNote.sql", "INSERT INTO notes (text) VALUES ({{text}}) RETURNING id");
    dir.write("app/code/queries/listNotes.sql", "SELECT id, text FROM notes ORDER BY id");
    dir.write("app/code/queries/sortedNotes.sql", "SELECT text FROM notes ORDER BY {{by: id, text}} {{dir: asc, desc}}");
    dir.write("app/code/pages/notes.ms",
              "get.list = function(req)\n"
              "    return json(db.query(\"listNotes\"))\n"
              "end function\n"
              "get.add = function(req)\n"
              "    rows = db.query(\"addNote\", {\"text\": req.query.t})\n"
              "    if rows isa error then return status(500, rows.message)\n"
              "    return json(rows[0])\n"
              "end function\n"
              "get.sorted = function(req)\n"
              "    rows = db.query(\"sortedNotes\", {\"by\": req.query.by, \"dir\": req.query.dir})\n"
              "    if rows isa error then return rows.message\n"
              "    return json(rows)\n"
              "end function\n"
              "get.bad = function(req)\n"
              "    r = db.query(req.query.q, [1])\n"
              "    if r isa error then return r.message\n"
              "    return \"ok\"\n"
              "end function\n"
              "get.version = function(req)\n"
              "    return \"v1 \" + db.query(\"listNotes\").len\n"
              "end function\n");
}

// Every file in data/ (but locks and the live database), by path.
std::map<std::string, std::string> dataFiles(const std::string& root) {
    std::map<std::string, std::string> out;
    fs::path data = fs::path(root) / "data";
    for (const auto& e : fs::recursive_directory_iterator(data)) {
        if (!e.is_regular_file()) continue;
        std::string rel = fs::relative(e.path(), data).string();
        if (rel.rfind(".fourfold/locks", 0) == 0 || rel.rfind(".fourfold/app.sqlite", 0) == 0) continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        out[rel] = s.str();
    }
    return out;
}

// The whole SQLite database, as text: every table's definition and rows.
std::string sqliteDump(const std::string& root) {
    auto db = Sqlite::open(root + "/data/.fourfold/app.sqlite");
    REQUIRE(db.ok());
    std::string out;
    auto tables = db.value()->prepareOnce("SELECT name, sql FROM sqlite_master WHERE type = 'table' ORDER BY name");
    REQUIRE(tables.ok());
    std::vector<std::string> names;
    while (tables.value()->step().value()) {
        names.push_back(tables.value()->text(0));
        out += tables.value()->text(1) + "\n";
    }
    for (const std::string& name : names) {
        auto rows = db.value()->prepareOnce("SELECT * FROM " + name + " ORDER BY 1");
        REQUIRE(rows.ok());
        while (rows.value()->step().value()) {
            out += name + ":";
            for (int c = 0; c < rows.value()->columnCount(); c++) out += " " + rows.value()->text(c);
            out += "\n";
        }
    }
    return out;
}

// The Postgres schema's tables and rows, as text.
std::string postgresDump(const PostgresSettings& settings) {
    auto db = Database::openPostgres(settings);
    REQUIRE(db.ok());
    auto q = [&](const std::string& sql) {
        std::vector<SqlProblem> problems;
        NamedQuery n;
        n.sql = prepareSql(sql, SqlDialect::Postgres, problems);
        auto rows = db.value()->query(n, {});
        REQUIRE_MESSAGE(rows.ok(), rows.error().message);
        return rows.value();
    };
    std::string out;
    Datum columns = q("SELECT table_name, column_name, data_type FROM information_schema.columns WHERE table_schema = "
                      "current_schema() ORDER BY table_name, ordinal_position");
    std::vector<std::string> tables;
    for (const Datum& row : columns.asList()) {
        out += toJson(row) + "\n";
        std::string t = row.asMap().at("table_name").asString();
        if (tables.empty() || tables.back() != t) tables.push_back(t);
    }
    for (const std::string& t : tables) out += t + ": " + toJson(q("SELECT * FROM " + t + " ORDER BY 1")) + "\n";
    db.value()->closeIdle();
    return out;
}

// The second batch: data files changed, a column added, a backfill.
void writeSecondBatch(const TempDir& dir) {
    dir.write("app/code/migrations/002_files.ms",
              "migrate = function\n"
              "    data.set \"counter\", 99\n"
              "    file.writeLines \"notes/readme.txt\", [\"changed by 002\"]\n"
              "    file.delete \"old.txt\"\n"
              "end function\n");
    dir.write("app/code/migrations/003_stars.sql", "ALTER TABLE notes ADD COLUMN stars INTEGER DEFAULT 0;\n");
    dir.write("app/code/migrations/004_backfill/migrate.ms",
              "import \"util\"\n"
              "migrate = function\n"
              "    for row in db.query(\"all\")\n"
              "        db.query(\"setStars\", {\"id\": row.id, \"stars\": util.starsFor(row.text)})\n"
              "    end for\n"
              "end function\n");
    dir.write("app/code/migrations/004_backfill/util.ms", "starsFor = function(text)\n    return text.len\nend function\n");
    dir.write("app/code/migrations/004_backfill/all.sql", "SELECT id, text FROM notes");
    dir.write("app/code/migrations/004_backfill/setStars.sql", "UPDATE notes SET stars = {{stars}} WHERE id = {{id}}");
}

}  // namespace

TEST_CASE("database: dev mode migrates, seeds, and serves named queries") {
    TempDir dir;
    writeNotesApp(dir);
    dir.write("app/code/seed.ms", "db.query \"addNote\", {\"text\": \"seeded\"}\n");
    Running r(dir.at("app"), devOptions());
    REQUIRE_MESSAGE(r.server->compiled(), r.err.str());
    CHECK(contains(r.out.str(), "Applied 1 migration"));
    CHECK(contains(r.out.str(), "then code/seed.ms"));

    std::string list = get(r.port(), "/notes/list");
    CHECK_EQ(statusOf(list), 200);
    CHECK_EQ(bodyText(list), "[{\"id\":1,\"text\":\"first\"},{\"id\":2,\"text\":\"seeded\"}]");
    std::string added = get(r.port(), "/notes/add?t=hello%27%3B%20DROP%20TABLE%20notes%3B--");
    CHECK_EQ(bodyText(added), "{\"id\":3}");
    CHECK(contains(bodyText(get(r.port(), "/notes/list")), "hello'; DROP TABLE notes;--"));

    // Mistakes come back as error values, for the code to handle.
    std::string nope = bodyText(get(r.port(), "/notes/bad?q=nope"));
    CHECK_MESSAGE(contains(nope, "there's no query named nope"), nope);
    CHECK(contains(bodyText(get(r.port(), "/notes/bad?q=addNote")), "must be a map, not a list"));

    // Name slots: a column and direction chosen by the visitor, from the
    // query's own list.
    CHECK(contains(bodyText(get(r.port(), "/notes/sorted?by=text&dir=desc")), "[{\"text\":\"seeded\"}"));
    CHECK(contains(bodyText(get(r.port(), "/notes/sorted?by=id&dir=asc")), "[{\"text\":\"first\"}"));
    CHECK_EQ(bodyText(get(r.port(), "/notes/sorted?by=id%3BDROP%20TABLE%20notes&dir=asc")),
             "db.query(\"sortedNotes\"): {{by}} must be id or text, not \"id;DROP TABLE notes\"");

    // A new migration and a changed query: applied after the recompile.
    dir.write("app/code/migrations/002_stars.sql", "ALTER TABLE notes ADD COLUMN stars INTEGER DEFAULT 5;\n");
    dir.write("app/code/queries/listNotes.sql", "SELECT id, text, stars FROM notes ORDER BY id");
    CompileReport report = r.server->instance().recompileNow();
    REQUIRE(report.ok);
    CHECK_EQ(report.migrated, std::vector<int>{2});
    CHECK_FALSE(report.backup.empty());
    CHECK(contains(bodyText(get(r.port(), "/notes/list")), "\"stars\":5"));

    // Editing an applied migration is refused, and the last good version
    // keeps serving.
    dir.write("app/code/migrations/002_stars.sql", "ALTER TABLE notes ADD COLUMN stars INTEGER DEFAULT 1;\n");
    report = r.server->instance().recompileNow();
    CHECK_FALSE(report.ok);
    bool changed = false;
    for (const Diagnostic& d : report.diagnostics) changed = changed || contains(d.message, "was changed after it was applied");
    CHECK(changed);
    CHECK_EQ(statusOf(get(r.port(), "/notes/list")), 200);

    // "Redo the last migration": back to the backup before it, and applied
    // again as it is now.
    MigrateResult redo = r.server->instance().redoLast();
    CHECK_MESSAGE(redo.ok, redo.message);
    CHECK(contains(bodyText(get(r.port(), "/notes/list")), "\"stars\":1"));
    MigrationStatus status = r.server->instance().migrationStatus();
    REQUIRE_EQ(status.items.size(), 2u);
    CHECK_EQ(status.items[1].state, "applied");

    // The dev tools' panel shows them, and its buttons work only from the
    // panel itself.
    std::string panel = get(r.port(), "/__dev/migrations");
    CHECK_EQ(statusOf(panel), 200);
    CHECK(contains(panel, "<td>002_stars.sql</td><td>applied</td>"));
    std::string post = "POST /__dev/migrations/reset HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n"
                       "Connection: close\r\n";
    CHECK_EQ(statusOf(rawFetch(r.port(), post + "Origin: http://evil.example\r\n\r\n")), 403);

    // "Reset dev data": empty, every migration again, then the seed.
    get(r.port(), "/notes/add?t=more");
    std::string reset = rawFetch(r.port(), post + "Origin: http://localhost\r\n\r\n");
    CHECK_EQ(statusOf(reset), 303);
    CHECK(contains(get(r.port(), "/__dev/migrations"), "Reset: ran 2 migration(s), then code/seed.ms."));
    CHECK_EQ(bodyText(get(r.port(), "/notes/list")),
             "[{\"id\":1,\"stars\":1,\"text\":\"first\"},{\"id\":2,\"stars\":1,\"text\":\"seeded\"}]");
}

namespace {

// The exit criterion, on one database: every step of a migration batch
// fails in turn, and each time the database and data files are exactly as
// they were.
void failureAtEachStep(const TempDir& dir, std::function<std::string()> dumpDatabase) {
    // Version 1, migrated.
    {
        std::ostringstream out, err;
        auto server = AppServer::start(dir.at("app"), offlineOptions(), out, err);
        REQUIRE_MESSAGE(server, err.str());
        REQUIRE(server->instance().migrateNow().ok);
    }
    dir.write("app/data/counter.json", "1\n");
    dir.write("app/data/old.txt", "keep me\n");
    dir.write("app/data/notes/readme.txt", "original\n");
    writeSecondBatch(dir);
    const std::map<std::string, std::string> filesBefore = dataFiles(dir.at("app"));
    const std::string databaseBefore = dumpDatabase();

    for (const char* step : {"backup", "before 2", "after 2", "before 3", "after 3", "before 4", "after 4", "commit"}) {
        CAPTURE(step);
        std::string failAt = step;
        ServerOptions options = offlineOptions();
        options.lifecycle.faults = [failAt](const std::string& s) -> Status {
            if (s == failAt) return makeError(ErrorCode::IO, "injected failure");
            return {};
        };
        std::ostringstream out, err;
        auto server = AppServer::start(dir.at("app"), options, out, err);
        REQUIRE_MESSAGE(server, err.str());
        CHECK_EQ(server->instance().pendingMigrations(), 3u);
        MigrateResult r = server->instance().migrateNow();
        CHECK_FALSE(r.ok);
        CHECK(contains(r.message, "injected failure"));
        server.reset();
        CHECK(dataFiles(dir.at("app")) == filesBefore);
        CHECK_EQ(dumpDatabase(), databaseBefore);
    }

    // And without a failure, all three apply.
    std::ostringstream out, err;
    auto server = AppServer::start(dir.at("app"), offlineOptions(), out, err);
    REQUIRE(server);
    MigrateResult r = server->instance().migrateNow();
    CHECK_MESSAGE(r.ok, r.message);
    CHECK_EQ(r.applied, (std::vector<int>{2, 3, 4}));
    server.reset();
    std::map<std::string, std::string> after = dataFiles(dir.at("app"));
    CHECK_EQ(after["counter.json"], "99");
    CHECK_FALSE(after.count("old.txt"));
    CHECK(contains(dumpDatabase(), "stars"));
}

}  // namespace

TEST_CASE("migrations: a failure at any step leaves SQLite and the data files exactly as they were") {
    TempDir dir;
    writeNotesApp(dir);
    std::string root = dir.at("app");
    failureAtEachStep(dir, [root] { return sqliteDump(root); });
}

TEST_CASE("migrations: a failure at any step leaves Postgres and the data files exactly as they were") {
    if (!TestPostgres::settings()) {
        MESSAGE("skipped: set FOURFOLD_TEST_PG_URL to run the Postgres tests");
        return;
    }
    TestPostgres pg;
    REQUIRE_MESSAGE(pg.ok(), pg.error());
    TempDir dir;
    writeNotesApp(dir);
    dir.write("app/private/config/app.ms", "config = {}\nconfig.db = {\"url\": \"" + pg.config().url +
                                               "\", \"schema\": \"" + pg.config().schema + "\"}\n");
    PostgresSettings settings = pg.config();
    failureAtEachStep(dir, [settings] { return postgresDump(settings); });
}

TEST_CASE("migrations: a long migration under load holds requests, then serves them on the new version") {
    TempDir dir;
    writeNotesApp(dir);
    dir.write("app/private/config/app.ms", "config = {}\nconfig.deploy = {\"migrationWaitSeconds\": 20}\n");
    ServerOptions options = serveOptions();
    options.migrateOnStart = true;
    Running r(dir.at("app"), options);
    CHECK_EQ(bodyText(get(r.port(), "/notes/version")), "v1 1");

    // Version 2: a migration that takes a while, and code that needs it.
    dir.write("app/code/migrations/002_slow/migrate.ms",
              "migrate = function\n"
              "    wait 1.5\n"
              "    db.query \"addColumn\"\n"
              "end function\n");
    dir.write("app/code/migrations/002_slow/addColumn.sql", "ALTER TABLE notes ADD COLUMN stars INTEGER DEFAULT 3");
    dir.write("app/code/queries/listNotes.sql", "SELECT id, text, stars FROM notes ORDER BY id");
    dir.write("app/code/pages/notes.ms",
              "get.version = function(req)\n"
              "    return \"v2 \" + db.query(\"listNotes\")[0].stars\n"
              "end function\n");
    CompileReport compiled = r.server->instance().recompileNow();
    REQUIRE(compiled.ok);
    CHECK_EQ(compiled.pendingMigrations, 1u);
    // Not swapped in yet: the old version serves until `fourfold migrate`.
    CHECK_EQ(bodyText(get(r.port(), "/notes/version")), "v1 1");

    // Load, from several threads, all through the migration.
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0}, v1{0}, v2{0}, held{0};
    std::vector<std::thread> load;
    for (int t = 0; t < 4; t++) {
        load.emplace_back([&] {
            while (!stop) {
                auto t0 = std::chrono::steady_clock::now();
                std::string response = get(r.port(), "/notes/version");
                auto took = std::chrono::steady_clock::now() - t0;
                std::string body = bodyText(response);
                if (statusOf(response) != 200) {
                    errors++;
                } else if (body == "v1 1") {
                    v1++;
                } else if (body == "v2 3") {
                    v2++;
                    if (took > std::chrono::milliseconds(700)) held++;
                } else {
                    errors++;
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // `fourfold migrate`, as an operator would run it: it asks the server.
    std::ostringstream out, err;
    int exit = cli::run({"migrate", dir.at("app")}, out, err);
    CHECK_MESSAGE(exit == 0, err.str());
    CHECK(contains(out.str(), "asked it to apply the migrations"));
    CHECK(contains(out.str(), "Applied 1 migration"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop = true;
    for (auto& t : load) t.join();

    CHECK_EQ(errors.load(), 0);   // nothing failed
    CHECK(v1.load() > 0);
    CHECK(v2.load() > 0);
    CHECK(held.load() > 0);       // some requests waited out the migration, then got v2
    CHECK_EQ(r.server->schemaStats().waiting, 0u);
}

TEST_CASE("migrations: requests that wait too long get a 503") {
    TempDir dir;
    writeNotesApp(dir);
    dir.write("app/code/pages/503.html", "<!DOCTYPE html><html><body><h1>Back soon</h1></body></html>");
    dir.write("app/private/config/app.ms", "config = {}\nconfig.deploy = {\"migrationWaitSeconds\": 0.3}\n");
    ServerOptions options = serveOptions();
    options.migrateOnStart = true;
    Running r(dir.at("app"), options);
    dir.write("app/code/migrations/002_slow/migrate.ms", "migrate = function\n    wait 1.5\nend function\n");
    REQUIRE_EQ(r.server->instance().recompileNow().pendingMigrations, 1u);

    std::thread migrating([&] { CHECK(r.server->instance().migrateNow().ok); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string response = get(r.port(), "/notes/version");
    migrating.join();
    CHECK_EQ(statusOf(response), 503);
    CHECK(contains(response, "Back soon"));
    CHECK(contains(response, "Retry-After: "));
    CHECK_EQ(statusOf(get(r.port(), "/notes/version")), 200);
}

TEST_CASE("migrations: serve waits for them, and refuses older code") {
    TempDir dir;
    writeNotesApp(dir);

    // A fresh install has every migration pending: serve won't start.
    {
        std::ostringstream out, err;
        CHECK_FALSE(AppServer::start(dir.at("app"), serveOptions(), out, err));
        CHECK(contains(err.str(), "1 migration is pending"));
    }
    // `fourfold migrate`, with no server running, applies them itself.
    {
        std::ostringstream out, err;
        CHECK_EQ(cli::run({"migrate", dir.at("app")}, out, err), 0);
        CHECK(contains(out.str(), "Applied 1 migration"));
        std::ostringstream again;
        CHECK_EQ(cli::run({"migrate", dir.at("app")}, again, err), 0);
        CHECK(contains(again.str(), "No migrations to apply"));
    }
    dir.write("app/code/migrations/002_more.sql", "CREATE TABLE more (x INTEGER);\n");
    {
        std::ostringstream out, err;
        CHECK_EQ(cli::run({"migrate", dir.at("app")}, out, err), 0);
    }
    // Code from before 002 was written: refused, unless overridden.
    fs::remove(dir.at("app/code/migrations/002_more.sql"));
    {
        std::ostringstream out, err;
        auto server = AppServer::start(dir.at("app"), serveOptions(), out, err);
        REQUIRE(server);   // it starts, but serves an error page: the code can't run on this data
        CHECK_FALSE(server->compiled());
        CHECK(contains(err.str(), "002_more was applied to this data, but this code doesn't have it"));
    }
    {
        ServerOptions options = serveOptions();
        options.allowOlderCode = true;
        std::ostringstream out, err;
        auto server = AppServer::start(dir.at("app"), options, out, err);
        REQUIRE(server);
        CHECK(server->compiled());
    }
}

TEST_CASE("backups: fourfold backup and restore") {
    TempDir dir;
    writeNotesApp(dir);
    std::ostringstream out, err;
    REQUIRE_EQ(cli::run({"migrate", dir.at("app")}, out, err), 0);
    dir.write("app/data/greeting.txt", "hello\n");

    std::ostringstream backedUp;
    REQUIRE_EQ(cli::run({"backup", dir.at("app")}, backedUp, err), 0);
    std::string name = test::textBetween(backedUp.str(), "Backup ", " is in");
    REQUIRE_FALSE(name.empty());

    dir.write("app/data/greeting.txt", "changed\n");
    dir.write("app/data/extra.txt", "added\n");

    std::ostringstream listed;
    CHECK_EQ(cli::run({"restore", dir.at("app"), "--list"}, listed, err), 0);
    CHECK(contains(listed.str(), name + "  (manual"));
    CHECK(contains(listed.str(), "before-migrations"));

    std::ostringstream restored;
    CHECK_EQ(cli::run({"restore", dir.at("app"), name}, restored, err), 0);
    CHECK(contains(restored.str(), "Restored " + name));
    std::map<std::string, std::string> files = dataFiles(dir.at("app"));
    CHECK_EQ(files["greeting.txt"], "hello\n");
    CHECK_FALSE(files.count("extra.txt"));
    CHECK(contains(sqliteDump(dir.at("app")), "notes: 1 first"));

    // Not while the app is running.
    {
        Running r(dir.at("app"), serveOptions());
        std::ostringstream o, e;
        CHECK_EQ(cli::run({"restore", dir.at("app"), name}, o, e), 1);
        CHECK(contains(e.str(), "stop its server first"));
        // A backup asks the server.
        std::ostringstream asked;
        CHECK_EQ(cli::run({"backup", dir.at("app")}, asked, e), 0);
        CHECK(contains(asked.str(), "asked it to take a backup"));
        CHECK(contains(asked.str(), "-manual"));
    }
}

TEST_CASE("database: the tasks sample runs, on a fresh copy") {
    TempDir dir;
    fs::path from = fs::path(FOURFOLD_TESTS_DIR) / ".." / "examples" / "tasks" / "code";
    fs::create_directories(dir.at("tasks/code"));
    fs::copy(from, dir.at("tasks/code"), fs::copy_options::recursive);
    Running r(dir.at("tasks"), devOptions());
    REQUIRE_MESSAGE(r.server->compiled(), r.err.str());
    CHECK(contains(r.out.str(), "Applied 3 migrations"));
    std::string page = get(r.port(), "/");
    CHECK_EQ(statusOf(page), 200);
    CHECK(contains(page, "3 open"));
    CHECK(contains(page, "<li class=\"urgent\">"));   // ranked by the seed
}

TEST_CASE("database: an app that doesn't use one gets no database file") {
    TempDir dir;
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>Hi</body></html>");
    dir.write("app/code/migrations/001_files.ms", "migrate = function\n    data.set \"ready\", 1\nend function\n");
    Running r(dir.at("app"), devOptions());
    REQUIRE_MESSAGE(r.server->compiled(), r.err.str());
    CHECK(contains(r.out.str(), "Applied 1 migration"));
    CHECK_EQ(statusOf(get(r.port(), "/")), 200);
    CHECK(fs::exists(dir.at("app/data/ready.json")));
    CHECK(fs::exists(dir.at("app/data/.fourfold/migrations.json")));
    CHECK_FALSE(fs::exists(dir.at("app/data/.fourfold/app.sqlite")));
}

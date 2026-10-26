#include "platform/sqlite.h"

#include <thread>

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

TEST_CASE("Sqlite: schema, insert, select, blobs and nulls") {
    TempDir dir;
    auto db = Sqlite::open(dir.at("t.db"));
    REQUIRE(db);
    Sqlite& s = *db.value();
    REQUIRE(s.exec("CREATE TABLE t (k TEXT PRIMARY KEY, n INTEGER, b BLOB, o TEXT)"));
    auto ins = s.statement("INSERT INTO t VALUES (?1, ?2, ?3, ?4)");
    REQUIRE(ins);
    std::string blob("a\0b", 3);
    ins.value()->bindText(1, "x").bindInt(2, 42).bindBlob(3, blob).bindTextOrNull(4, std::nullopt);
    REQUIRE(ins.value()->run());
    CHECK_EQ(s.changes(), 1);

    auto again = s.statement("INSERT INTO t VALUES (?1, ?2, ?3, ?4)");
    again.value()->bindText(1, "x").bindInt(2, 1).bindBlob(3, "").bindNull(4);
    auto dup = again.value()->run();
    REQUIRE_FALSE(dup);
    CHECK(dup.error().code == ErrorCode::Conflict);

    auto sel = s.statement("SELECT n, b, o FROM t WHERE k = ?1");
    sel.value()->bindText(1, "x");
    auto row = sel.value()->step();
    REQUIRE(row);
    REQUIRE(row.value());
    CHECK_EQ(sel.value()->integer(0), 42);
    CHECK_EQ(sel.value()->text(1), blob);
    CHECK(sel.value()->isNull(2));
    CHECK_FALSE(sel.value()->step().value());
}

TEST_CASE("Sqlite: transactions commit or roll back") {
    TempDir dir;
    auto db = Sqlite::open(dir.at("t.db")).value();
    REQUIRE(db->exec("CREATE TABLE t (n INTEGER)"));
    REQUIRE(db->transaction([&]() -> Status { return db->exec("INSERT INTO t VALUES (1)"); }));
    Status failed = db->transaction([&]() -> Status {
        (void)db->exec("INSERT INTO t VALUES (2)");
        return makeError(ErrorCode::Invalid, "no");
    });
    CHECK_FALSE(failed);
    auto count = db->statement("SELECT count(*) FROM t").value();
    REQUIRE(count->step().value());
    CHECK_EQ(count->integer(0), 1);
}

TEST_CASE("Sqlite: two connections on one file, as two processes would") {
    TempDir dir;
    auto a = Sqlite::open(dir.at("t.db")).value();
    REQUIRE(a->exec("CREATE TABLE c (n INTEGER); INSERT INTO c VALUES (0)"));
    auto bump = [&dir] {
        auto db = Sqlite::open(dir.at("t.db")).value();
        for (int i = 0; i < 200; i++) {
            Status s = db->transaction([&]() -> Status { return db->exec("UPDATE c SET n = n + 1"); });
            if (!s) return false;
        }
        return true;
    };
    bool ok1 = false, ok2 = false;
    std::thread t1([&] { ok1 = bump(); });
    std::thread t2([&] { ok2 = bump(); });
    t1.join();
    t2.join();
    CHECK(ok1);
    CHECK(ok2);
    auto n = a->statement("SELECT n FROM c").value();
    REQUIRE(n->step().value());
    CHECK_EQ(n->integer(0), 400);
}

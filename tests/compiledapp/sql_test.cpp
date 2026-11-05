// The SQL scanner (overview §3, SQL dialects): placeholders found and
// rewritten for each database, and left alone inside strings, quoted names
// and comments; mistakes reported; values bound from lists and maps.
#include "compiledapp/sql.h"

#include "doctest.h"

using namespace fourfold;

namespace {

PreparedSql prep(const std::string& sql, SqlDialect d, std::vector<SqlProblem>* problems = nullptr) {
    std::vector<SqlProblem> local;
    PreparedSql out = prepareSql(sql, d, problems ? *problems : local);
    if (!problems) {
        std::string all;
        for (const SqlProblem& p : local) all += std::to_string(p.line) + ": " + p.message + "\n";
        CHECK_MESSAGE(local.empty(), all);
    }
    return out;
}

std::string firstProblem(const std::string& sql, SqlDialect d) {
    std::vector<SqlProblem> problems;
    prepareSql(sql, d, problems);
    return problems.empty() ? "" : std::to_string(problems[0].line) + ": " + problems[0].message;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

Datum str(const char* s) { return Datum::string(s); }
Datum num(double x) { return Datum::number(x); }
Datum::ListItems list(std::initializer_list<Datum> items) { return Datum::ListItems(items); }

constexpr SqlDialect kSqlite = SqlDialect::Sqlite;
constexpr SqlDialect kPg = SqlDialect::Postgres;

}  // namespace

TEST_CASE("sql: placeholders become each database's parameters") {
    PreparedSql s = prep("SELECT * FROM users WHERE email = {{0}}", kSqlite);
    CHECK_EQ(s.sql, "SELECT * FROM users WHERE email = ?1");
    CHECK_EQ(s.keys, std::vector<std::string>{"0"});
    CHECK(s.allIndexes());
    CHECK_EQ(s.statements, 1);

    PreparedSql p = prep("SELECT * FROM users WHERE email = {{0}}", kPg);
    CHECK_EQ(p.sql, "SELECT * FROM users WHERE email = $1");

    // A repeated key is one parameter; keys are in order of first use.
    PreparedSql r = prep("SELECT 1 WHERE owner = {{ user }} OR editor = {{user}} AND t > {{since}}", kPg);
    CHECK_EQ(r.sql, "SELECT 1 WHERE owner = $1 OR editor = $1 AND t > $2");
    CHECK_EQ(r.keys, std::vector<std::string>{"user", "since"});
    CHECK_FALSE(r.allIndexes());

    // Indexes in any order.
    PreparedSql o = prep("INSERT INTO t (b, a) VALUES ({{1}}, {{0}})", kSqlite);
    CHECK_EQ(o.sql, "INSERT INTO t (b, a) VALUES (?1, ?2)");
    CHECK_EQ(o.keys, (std::vector<std::string>{"1", "0"}));

    // Never glued to a following word.
    CHECK_EQ(prep("SELECT {{x}}abc", kPg).sql, "SELECT $1 abc");
}

TEST_CASE("sql: strings, quoted names and comments are left alone") {
    // Postgres syntax that would have looked like placeholders with other designs.
    std::vector<SqlProblem> problems;
    PreparedSql q = prepareSql("SELECT $$it's$$, E'a\\'b', x::int, arr[1:2], d ? 'k' -- {{c}}\n"
                               "/* {{d}} /* nested */ {{e}} */ FROM t WHERE id = {{id}}",
                               kPg, problems);
    CHECK(problems.empty());
    CHECK_EQ(q.keys, std::vector<std::string>{"id"});
    CHECK(contains(q.sql, "id = $1"));
    CHECK(contains(q.sql, "x::int"));

    std::vector<SqlProblem> sqliteProblems;
    PreparedSql s = prepareSql("SELECT 'a''b', \"we\"\"ird\", [old name], `x` -- {{c}}\n/* {{d}} */ FROM t "
                               "WHERE id = {{id}}",
                               kSqlite, sqliteProblems);
    CHECK(sqliteProblems.empty());
    CHECK_EQ(s.keys, std::vector<std::string>{"id"});
}

TEST_CASE("sql: placeholders inside quotes are mistakes") {
    CHECK(contains(firstProblem("SELECT * FROM t WHERE name = '{{name}}'", kSqlite), "can't go inside quotes"));
    CHECK(contains(firstProblem("SELECT \"{{col}}\" FROM t", kPg), "can't go inside quotes"));
    CHECK(contains(firstProblem("SELECT E'{{x}}'", kPg), "can't go inside quotes"));
    CHECK(contains(firstProblem("SELECT $body$\n {{x}} $body$", kPg), "can't go inside quotes"));
    CHECK(contains(firstProblem("SELECT [{{x}}] FROM t", kSqlite), "can't go inside quotes"));
    // The line is the placeholder's.
    CHECK_EQ(firstProblem("SELECT 'a\n\n{{x}}'", kSqlite).substr(0, 2), "3:");
    // A lone brace is just SQL.
    CHECK_EQ(firstProblem("SELECT '{', '}', '{x}'", kSqlite), "");
}

TEST_CASE("sql: malformed placeholders and the databases' own parameters") {
    CHECK(contains(firstProblem("SELECT {{01}}", kSqlite), "isn't a placeholder"));
    CHECK(contains(firstProblem("SELECT {{1a}}", kSqlite), "isn't a placeholder"));
    CHECK(contains(firstProblem("SELECT {{a.b}}", kSqlite), "isn't a placeholder"));
    CHECK(contains(firstProblem("SELECT {{}}", kSqlite), "isn't a placeholder"));
    CHECK(contains(firstProblem("SELECT {{x", kSqlite), "no }}"));
    CHECK(contains(firstProblem("SELECT {{x\n}}", kSqlite), "no }}"));
    CHECK(contains(firstProblem("SELECT * FROM t WHERE a = ?", kSqlite), "SQLite would read ?"));
    CHECK(contains(firstProblem("SELECT * FROM t WHERE a = :a", kSqlite), "as a parameter"));
    CHECK(contains(firstProblem("SELECT * FROM t WHERE a = @a", kSqlite), "as a parameter"));
    CHECK(contains(firstProblem("SELECT * FROM t WHERE a = $1", kPg), "$1-style"));
    // Fine on the other database.
    CHECK_EQ(firstProblem("SELECT data ? 'k' FROM t", kPg), "");
    CHECK(contains(firstProblem("SELECT 'unclosed", kSqlite), "isn't closed"));
    CHECK(contains(firstProblem("SELECT 1 /* unclosed", kPg), "isn't closed"));
    CHECK(contains(firstProblem("SELECT $x$ unclosed", kPg), "isn't closed"));
}

TEST_CASE("sql: statements are counted") {
    CHECK_EQ(prep("SELECT 1", kSqlite).statements, 1);
    CHECK_EQ(prep("SELECT 1;", kSqlite).statements, 1);
    CHECK_EQ(prep("SELECT 1; -- done\n", kSqlite).statements, 1);
    CHECK_EQ(prep("CREATE TABLE a (x); CREATE TABLE b (y);", kSqlite).statements, 2);
    CHECK_EQ(prep("SELECT ';'", kSqlite).statements, 1);
    CHECK_EQ(prep("-- nothing\n/* at all */", kPg).statements, 0);
    CHECK_EQ(prep("CREATE FUNCTION f() RETURNS int AS $$ SELECT 1; SELECT 2; $$ LANGUAGE sql;", kPg).statements, 1);
}

TEST_CASE("sql: binding values from lists and maps") {
    PreparedSql idx = prep("SELECT {{1}}, {{0}}", kSqlite);
    auto list = bindValues(idx, Datum::list({Datum::string("a"), Datum::number(2), Datum::string("extra")}));
    REQUIRE(list.ok());
    CHECK_EQ(list.value(), (std::vector<Datum>{Datum::number(2), Datum::string("a")}));

    auto map = bindValues(idx, Datum::map({{"0", Datum::string("zero")}, {"1", Datum()}, {"other", Datum()}}));
    REQUIRE(map.ok());
    CHECK_EQ(map.value(), (std::vector<Datum>{Datum(), Datum::string("zero")}));

    auto short1 = bindValues(idx, Datum::list({Datum::string("a")}));
    REQUIRE_FALSE(short1.ok());
    CHECK(contains(short1.error().message, "no value for {{1}}: the list has 1 item"));

    PreparedSql named = prep("SELECT {{title}}, {{0}}", kPg);
    auto wrongKind = bindValues(named, Datum::list({Datum::string("a")}));
    REQUIRE_FALSE(wrongKind.ok());
    CHECK(contains(wrongKind.error().message, "must be a map"));
    auto missing = bindValues(named, Datum::map({{"0", Datum::number(1)}, {"titel", Datum::string("typo")}}));
    REQUIRE_FALSE(missing.ok());
    CHECK(contains(missing.error().message, "no value for {{title}}"));

    PreparedSql none = prep("SELECT 1", kSqlite);
    CHECK(bindValues(none, Datum()).ok());
    CHECK(bindValues(none, Datum::list({})).ok());
    CHECK_FALSE(bindValues(idx, Datum()).ok());
    CHECK_FALSE(bindValues(idx, Datum::string("x")).ok());
}

TEST_CASE("sql: name slots pick from their own list") {
    PreparedSql s = prep("SELECT id, title FROM notes WHERE owner = {{user}}\n"
                         "ORDER BY {{sort: title, created, notes.stars}} {{ dir : asc, desc }} LIMIT {{n}}",
                         kPg);
    CHECK_EQ(s.keys, (std::vector<std::string>{"user", "n"}));
    REQUIRE_EQ(s.slots.size(), 2u);
    CHECK_EQ(s.slots[0].options, (std::vector<std::string>{"title", "created", "notes.stars"}));
    CHECK_EQ(s.slots[1].key, "dir");
    CHECK_EQ(s.allKeys(), (std::vector<std::string>{"user", "n", "sort", "dir"}));
    CHECK(s.sql.empty());
    REQUIRE_EQ(s.pieces.size(), 3u);

    auto bound = bindQuery(s, Datum::map({{"user", str("u1")}, {"sort", str("Created")}, {"dir", str("DESC")},
                                          {"n", num(10)}}));
    REQUIRE_MESSAGE(bound.ok(), bound.error().message);
    CHECK_EQ(bound.value().sql, "SELECT id, title FROM notes WHERE owner = $1\nORDER BY created desc LIMIT $2");
    CHECK_EQ(bound.value().params, (std::vector<Datum>{str("u1"), num(10)}));

    // Anything not on the list is refused: the SQL is never anything else.
    for (Datum bad : {str("title; DROP TABLE notes"), str("password"), str(""), num(1), Datum()}) {
        auto refused = bindQuery(s, Datum::map({{"user", str("u1")}, {"sort", bad}, {"dir", str("asc")},
                                                {"n", num(1)}}));
        REQUIRE_FALSE(refused.ok());
        CHECK(contains(refused.error().message, "{{sort}} must be title, created or notes.stars"));
    }
    auto missing = bindQuery(s, Datum::map({{"user", str("u1")}, {"n", num(1)}, {"dir", str("asc")}}));
    REQUIRE_FALSE(missing.ok());
    CHECK(contains(missing.error().message, "no value for {{sort}}"));

    // Index slots work with a list.
    PreparedSql listed = prep("SELECT * FROM t ORDER BY {{0: a, b}}", kSqlite);
    auto fromList = bindQuery(listed, Datum::list(list({str("b")})));
    REQUIRE(fromList.ok());
    CHECK_EQ(fromList.value().sql, "SELECT * FROM t ORDER BY b");
    CHECK(listed.allIndexes());

    // A query without slots binds as before.
    auto plain = bindQuery(prep("SELECT {{0}}", kSqlite), Datum::list(list({num(1)})));
    REQUIRE(plain.ok());
    CHECK_EQ(plain.value().sql, "SELECT ?1");
}

TEST_CASE("sql: mistakes in name slots") {
    CHECK(contains(firstProblem("SELECT * FROM t ORDER BY {{sort: title, 1x}}", kSqlite), "\"1x\" isn't a plain name"));
    CHECK(contains(firstProblem("SELECT * FROM t ORDER BY {{sort: title;drop}}", kSqlite), "isn't a plain name"));
    CHECK(contains(firstProblem("SELECT * FROM t ORDER BY {{sort: }}", kSqlite), "\"\" isn't a plain name"));
    CHECK(contains(firstProblem("SELECT * FROM t ORDER BY {{sort: a,, b}}", kSqlite), "isn't a plain name"));
    CHECK(contains(firstProblem("SELECT * FROM t ORDER BY {{: a}}", kSqlite), "isn't a name slot"));
    CHECK(contains(firstProblem("SELECT {{x}} FROM t ORDER BY {{x: a}}", kSqlite), "is a value elsewhere"));
    CHECK(contains(firstProblem("SELECT * FROM t ORDER BY {{x: a}}, {{x}}", kSqlite), "is a name slot elsewhere"));
    CHECK(contains(firstProblem("SELECT {{x: a}}, {{x: b}}", kSqlite), "different names"));
    CHECK_EQ(firstProblem("SELECT {{x: a, b}}, {{x: a, b}}", kSqlite), "");
    CHECK(contains(firstProblem("SELECT '{{x: a}}'", kSqlite), "can't go inside quotes"));
}

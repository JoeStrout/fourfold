// Sqlite: a thin wrapper over one SQLite connection (Arch §5, foundation).
//
// A connection may be used by one thread at a time (SQLite's multi-thread
// mode); stores give each worker that needs one its own, or guard one with a
// mutex. Opened in WAL mode with a busy timeout, so readers don't block the
// writer, and other processes (CGI mode) wait their turn instead of failing.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace fourfold {

class SqliteStatement {
public:
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    ~SqliteStatement();

    // Parameters are numbered from 1. Each returns *this, for chaining.
    SqliteStatement& bindInt(int index, int64_t value);
    SqliteStatement& bindText(int index, std::string_view text);
    SqliteStatement& bindBlob(int index, std::string_view bytes);
    SqliteStatement& bindNull(int index);
    // Text, or null for nullopt.
    SqliteStatement& bindTextOrNull(int index, const std::optional<std::string>& value);

    // Runs to the next row: true if there is one, false when done.
    Result<bool> step();
    // Steps until done, ignoring any rows.
    Status run();

    // Columns are numbered from 0, for the current row.
    bool isNull(int column) const;
    int64_t integer(int column) const;
    std::string text(int column) const;   // also for blobs

private:
    friend class Sqlite;
    SqliteStatement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
    void reset();

    sqlite3* db_;
    sqlite3_stmt* stmt_;
    std::string bindError_;   // the first failed bind, reported by step()
};

class Sqlite {
public:
    // Opens (creating) the database at `path`.
    static Result<std::unique_ptr<Sqlite>> open(const std::string& path,
                                                std::chrono::milliseconds busyTimeout = std::chrono::seconds(5));
    ~Sqlite();

    Sqlite(const Sqlite&) = delete;
    Sqlite& operator=(const Sqlite&) = delete;

    // Runs SQL with no parameters (one or more statements), e.g. a schema.
    Status exec(const std::string& sql);

    // A prepared statement for `sql`, cleared of earlier bindings and ready
    // to run. Statements are cached by their text; the reference stays valid
    // until the next call with the same text.
    Result<SqliteStatement*> statement(const std::string& sql);

    // Runs `body` in a write transaction (BEGIN IMMEDIATE), committing if it
    // returns success and rolling back otherwise.
    Status transaction(const std::function<Status()>& body);

    // Rows changed by the last INSERT, UPDATE or DELETE.
    int64_t changes() const;

private:
    explicit Sqlite(sqlite3* db) : db_(db) {}
    Error error(const std::string& what) const;

    sqlite3* db_;
    std::unordered_map<std::string, std::unique_ptr<SqliteStatement>> cache_;
};

}  // namespace fourfold

// Database: the app's SQL database (overview §3, Middle tier and Tier 2;
// Arch §5 appdata). SQLite, a file in data/.fourfold/, or Postgres, which
// the host config names. Both run the same named queries.
//
// App code can only run named queries (NamedQuery, made by the compiler
// from code/queries/), with values bound as parameters, never pasted into
// the SQL. Every query's result is its rows, as a list of maps by column
// name; a statement that returns no rows gives an empty list (INSERT ...
// RETURNING id gives the new row's id).
//
// Values in: null, numbers (whole ones as integers), strings, and lists and
// maps as JSON text. Values out: null, numbers, strings; on Postgres,
// booleans as 1 and 0, and json/jsonb columns parsed. Binary data (BLOB,
// bytea) can't come back, since MiniScript strings can't hold it.
//
// Everything here blocks: worker threads only. A Database is a pool,
// usable from several threads at once; a DbConnection is one connection,
// used by one thread at a time.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/datum.h"
#include "base/result.h"
#include "compiledapp/compiled_app.h"

namespace fourfold {

struct DbLimits {
    // How long one query may run before it's stopped.
    std::chrono::milliseconds queryTimeout{10'000};
    // The most rows, and bytes of text, one query may return.
    size_t maxRows = 10'000;
    size_t maxBytes = 16 * 1024 * 1024;
};

// Runs named queries. Errors say what went wrong, with the database's own
// message ("UNIQUE constraint failed: users.email"), for the developer.
class QueryRunner {
public:
    virtual ~QueryRunner() = default;
    // `params` are in the query's parameter order (bindValues).
    virtual Result<Datum> query(const NamedQuery& query, const std::vector<Datum>& params) = 0;
};

// One connection of a Database's, held until destroyed.
class DbConnection : public QueryRunner {
public:
    // A transaction: everything until commit or rollback is one change. A
    // connection let go with a transaction open rolls it back.
    virtual Status begin() = 0;
    virtual Status commit() = 0;
    virtual Status rollback() = 0;
    // Runs SQL with no values (a .sql migration: any number of statements).
    virtual Status exec(const std::string& sql) = 0;
};

// Postgres's connection settings (from config.db).
struct PostgresSettings {
    std::string url;        // "postgres://user@host:5432/dbname", or key=value pairs
    std::string password;   // from an engine-only secret; empty for none
    std::string schema;     // a schema to work in (search_path); empty: the default
};

class Database : public QueryRunner {
public:
    // SQLite: the database file at `path` (a host path, created if missing).
    static Result<std::unique_ptr<Database>> openSqlite(const std::string& path, DbLimits limits = {},
                                                        size_t poolSize = 4);
    // Postgres. Fails (Invalid) if this build has no Postgres support.
    static Result<std::unique_ptr<Database>> openPostgres(const PostgresSettings& settings, DbLimits limits = {},
                                                          size_t poolSize = 4);
    // Whether this build of Fourfold can connect to Postgres (it was built
    // with libpq).
    static bool postgresSupported();

    virtual SqlDialect dialect() const = 0;

    // A connection of its own, from the pool, until it's destroyed (for a
    // transaction across several queries: a migration's).
    virtual Result<std::unique_ptr<DbConnection>> connection() = 0;

    // Closes the pool's idle connections (before the database file is
    // replaced, e.g. by a restore); they reopen as needed.
    virtual void closeIdle() = 0;
};

// A Database opened on first use by `open` (so an app that never uses one
// gets no database file). If opening fails, the call fails, and the next
// one tries again.
std::unique_ptr<Database> makeLazyDatabase(SqlDialect dialect,
                                           std::function<Result<std::unique_ptr<Database>>()> open);

}  // namespace fourfold

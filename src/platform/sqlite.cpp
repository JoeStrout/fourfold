#include "platform/sqlite.h"

#include "sqlite3.h"

namespace fourfold {

namespace {

Error sqliteError(sqlite3* db, int rc, const std::string& what) {
    ErrorCode code = ErrorCode::IO;
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) code = ErrorCode::WouldBlock;
    if (rc == SQLITE_CONSTRAINT) code = ErrorCode::Conflict;
    if (rc == SQLITE_TOOBIG) code = ErrorCode::TooLarge;
    if (rc == SQLITE_MISUSE || rc == SQLITE_RANGE) code = ErrorCode::Internal;
    std::string message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    return makeError(code, what + ": " + message);
}

}  // namespace

SqliteStatement::~SqliteStatement() { sqlite3_finalize(stmt_); }

void SqliteStatement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    bindError_.clear();
}

SqliteStatement& SqliteStatement::bindInt(int index, int64_t value) {
    int rc = sqlite3_bind_int64(stmt_, index, value);
    if (rc != SQLITE_OK && bindError_.empty()) bindError_ = sqlite3_errstr(rc);
    return *this;
}

SqliteStatement& SqliteStatement::bindText(int index, std::string_view text) {
    int rc = sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    if (rc != SQLITE_OK && bindError_.empty()) bindError_ = sqlite3_errstr(rc);
    return *this;
}

SqliteStatement& SqliteStatement::bindBlob(int index, std::string_view bytes) {
    // A zero-length blob, not null, for empty bytes.
    int rc = sqlite3_bind_blob64(stmt_, index, bytes.empty() ? "" : bytes.data(), bytes.size(), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK && bindError_.empty()) bindError_ = sqlite3_errstr(rc);
    return *this;
}

SqliteStatement& SqliteStatement::bindNull(int index) {
    int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK && bindError_.empty()) bindError_ = sqlite3_errstr(rc);
    return *this;
}

SqliteStatement& SqliteStatement::bindTextOrNull(int index, const std::optional<std::string>& value) {
    return value ? bindText(index, std::string_view(*value)) : bindNull(index);
}

Result<bool> SqliteStatement::step() {
    if (!bindError_.empty()) return makeError(ErrorCode::Internal, "SQLite bind: " + bindError_);
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    Error e = sqliteError(db_, rc, "SQLite");
    sqlite3_reset(stmt_);
    return e;
}

Status SqliteStatement::run() {
    while (true) {
        auto row = step();
        if (!row) return row.error();
        if (!row.value()) return {};
    }
}

bool SqliteStatement::isNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

int64_t SqliteStatement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }

std::string SqliteStatement::text(int column) const {
    const void* p = sqlite3_column_blob(stmt_, column);
    int n = sqlite3_column_bytes(stmt_, column);
    if (!p || n <= 0) return {};
    return std::string(static_cast<const char*>(p), static_cast<size_t>(n));
}

Result<std::unique_ptr<Sqlite>> Sqlite::open(const std::string& path, std::chrono::milliseconds busyTimeout) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW,
                             nullptr);
    if (rc != SQLITE_OK) {
        Error e = sqliteError(db, rc, "SQLite open");
        sqlite3_close(db);
        return e;
    }
    std::unique_ptr<Sqlite> out(new Sqlite(db));
    sqlite3_busy_timeout(db, static_cast<int>(busyTimeout.count()));
    sqlite3_extended_result_codes(db, 0);
    if (Status s = out->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"); !s) {
        return s.error();
    }
    return out;
}

Sqlite::~Sqlite() {
    cache_.clear();   // finalize statements before closing
    sqlite3_close(db_);
}

Error Sqlite::error(const std::string& what) const { return sqliteError(db_, sqlite3_errcode(db_), what); }

Status Sqlite::exec(const std::string& sql) {
    char* message = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string m = message ? message : sqlite3_errstr(rc);
        sqlite3_free(message);
        Error e = sqliteError(nullptr, rc, "SQLite");
        e.message = "SQLite: " + m;
        return e;
    }
    return {};
}

Result<SqliteStatement*> Sqlite::statement(const std::string& sql) {
    auto it = cache_.find(sql);
    if (it != cache_.end()) {
        it->second->reset();
        return it->second.get();
    }
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v3(db_, sql.c_str(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT, &stmt,
                                nullptr);
    if (rc != SQLITE_OK) return sqliteError(db_, rc, "SQLite prepare");
    auto owned = std::unique_ptr<SqliteStatement>(new SqliteStatement(db_, stmt));
    SqliteStatement* p = owned.get();
    cache_.emplace(sql, std::move(owned));
    return p;
}

Status Sqlite::transaction(const std::function<Status()>& body) {
    if (Status s = exec("BEGIN IMMEDIATE"); !s) return s;
    Status result = body();
    if (result) {
        if (Status s = exec("COMMIT"); !s) {
            (void)exec("ROLLBACK");
            return s;
        }
        return {};
    }
    (void)exec("ROLLBACK");
    return result;
}

int64_t Sqlite::changes() const { return sqlite3_changes64(db_); }

}  // namespace fourfold

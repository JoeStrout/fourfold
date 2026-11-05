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

SqliteStatement& SqliteStatement::bindDouble(int index, double value) {
    int rc = sqlite3_bind_double(stmt_, index, value);
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

int SqliteStatement::columnCount() const { return sqlite3_column_count(stmt_); }

std::string SqliteStatement::columnName(int column) const {
    const char* name = sqlite3_column_name(stmt_, column);
    return name ? name : "";
}

SqliteStatement::Type SqliteStatement::type(int column) const {
    switch (sqlite3_column_type(stmt_, column)) {
        case SQLITE_INTEGER: return Type::Integer;
        case SQLITE_FLOAT: return Type::Real;
        case SQLITE_TEXT: return Type::Text;
        case SQLITE_BLOB: return Type::Blob;
        default: return Type::Null;
    }
}

double SqliteStatement::real(int column) const { return sqlite3_column_double(stmt_, column); }

bool SqliteStatement::readOnly() const { return sqlite3_stmt_readonly(stmt_) != 0; }

void SqliteStatement::finish() { sqlite3_reset(stmt_); }

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

Result<std::unique_ptr<SqliteStatement>> Sqlite::prepareOnce(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    int rc = sqlite3_prepare_v3(db_, sql.c_str(), static_cast<int>(sql.size()), 0, &stmt, &tail);
    if (rc != SQLITE_OK) return sqliteError(db_, rc, "SQLite");
    auto owned = std::unique_ptr<SqliteStatement>(new SqliteStatement(db_, stmt));
    if (!stmt) return makeError(ErrorCode::Invalid, "SQLite: there's no statement to run");
    // Anything after it must be space and comments only.
    if (tail && *tail) {
        sqlite3_stmt* rest = nullptr;
        rc = sqlite3_prepare_v2(db_, tail, -1, &rest, nullptr);
        bool another = rc != SQLITE_OK || rest != nullptr;
        sqlite3_finalize(rest);
        if (another) return makeError(ErrorCode::Invalid, "SQLite: only one statement may be run at a time");
    }
    return owned;
}

int Sqlite::progress(void* self) {
    auto* db = static_cast<Sqlite*>(self);
    return db->deadline_ && std::chrono::steady_clock::now() > *db->deadline_ ? 1 : 0;
}

void Sqlite::setDeadline(std::optional<std::chrono::steady_clock::time_point> deadline) {
    deadline_ = deadline;
    if (deadline) {
        sqlite3_progress_handler(db_, 1000, &Sqlite::progress, this);
    } else {
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
    }
}

namespace {

// Copies `from` into `to` with the online backup API, a step at a time.
Status copyDatabase(sqlite3* from, sqlite3* to, const std::string& what) {
    sqlite3_backup* backup = sqlite3_backup_init(to, "main", from, "main");
    if (!backup) return sqliteError(to, sqlite3_errcode(to), what);
    int rc;
    do {
        rc = sqlite3_backup_step(backup, 256);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) sqlite3_sleep(20);
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE) return sqliteError(nullptr, rc, what);
    return {};
}

}  // namespace

Status Sqlite::backupTo(const std::string& path) {
    sqlite3* dest = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW,
                             nullptr);
    if (rc != SQLITE_OK) {
        Error e = sqliteError(dest, rc, "SQLite backup");
        sqlite3_close(dest);
        return e;
    }
    Status s = copyDatabase(db_, dest, "SQLite backup");
    sqlite3_close(dest);
    return s;
}

Status Sqlite::restoreFrom(const std::string& path) {
    sqlite3* src = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &src, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, nullptr);
    if (rc != SQLITE_OK) {
        Error e = sqliteError(src, rc, "SQLite restore");
        sqlite3_close(src);
        return e;
    }
    cache_.clear();   // statements over the old schema
    Status s = copyDatabase(src, db_, "SQLite restore");
    sqlite3_close(src);
    return s;
}

}  // namespace fourfold

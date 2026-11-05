// Database over SQLite: a pool of connections to one file (in WAL mode, so
// readers don't wait for the writer).

#include "appdata/database.h"

#include <cmath>
#include <condition_variable>
#include <mutex>

#include "base/utf8.h"
#include "db_common.h"
#include "platform/sqlite.h"

namespace fourfold {

namespace {

// Binds `value` as parameter `index` (from 1).
void bind(SqliteStatement& st, int index, const Datum& value) {
    switch (value.kind()) {
        case Datum::Kind::Null: st.bindNull(index); break;
        case Datum::Kind::Number: {
            double x = value.asNumber();
            if (isWholeNumber(x)) {
                st.bindInt(index, static_cast<int64_t>(x));
            } else {
                st.bindDouble(index, x);
            }
            break;
        }
        case Datum::Kind::String: st.bindText(index, value.asString()); break;
        case Datum::Kind::List:
        case Datum::Kind::Map: st.bindText(index, toJson(value)); break;
    }
}

Result<Datum> runQuery(Sqlite& db, const NamedQuery& query, const std::vector<Datum>& params,
                       const DbLimits& limits) {
    auto prepared = db.statement(query.sql.sql);
    if (!prepared) return prepared.error();
    SqliteStatement& st = *prepared.value();
    for (size_t i = 0; i < params.size(); i++) bind(st, static_cast<int>(i + 1), params[i]);

    db.setDeadline(std::chrono::steady_clock::now() + limits.queryTimeout);
    RowCollector rows(limits);
    std::vector<std::string> names;
    while (true) {
        auto more = st.step();
        if (!more) {
            db.setDeadline(std::nullopt);
            Error e = more.error();
            if (e.message.find("interrupted") != std::string::npos) return timedOut(limits);
            return e;
        }
        if (!more.value()) break;
        if (names.empty()) {
            for (int c = 0; c < st.columnCount(); c++) names.push_back(st.columnName(c));
        }
        Datum::MapItems row;
        for (int c = 0; c < static_cast<int>(names.size()); c++) {
            switch (st.type(c)) {
                case SqliteStatement::Type::Null: row[names[c]] = Datum(); break;
                case SqliteStatement::Type::Integer:
                    row[names[c]] = Datum::number(static_cast<double>(st.integer(c)));
                    break;
                case SqliteStatement::Type::Real: row[names[c]] = Datum::number(st.real(c)); break;
                case SqliteStatement::Type::Text: {
                    auto text = textValue(st.text(c), names[c]);
                    if (!text) {
                        st.finish();
                        db.setDeadline(std::nullopt);
                        return text.error();
                    }
                    row[names[c]] = std::move(text).value();
                    break;
                }
                case SqliteStatement::Type::Blob:
                    st.finish();
                    db.setDeadline(std::nullopt);
                    return binaryColumn(names[c]);
            }
        }
        if (Status s = rows.add(Datum::map(std::move(row))); !s) {
            st.finish();
            db.setDeadline(std::nullopt);
            return s.error();
        }
    }
    db.setDeadline(std::nullopt);
    return rows.result();
}

class SqlitePool;

class SqliteConnection : public DbConnection {
public:
    SqliteConnection(SqlitePool& pool, std::unique_ptr<Sqlite> db) : pool_(pool), db_(std::move(db)) {}
    ~SqliteConnection() override;

    Result<Datum> query(const NamedQuery& query, const std::vector<Datum>& params) override;
    Status begin() override {
        if (Status s = db_->exec("BEGIN IMMEDIATE"); !s) return s;
        inTransaction_ = true;
        return {};
    }
    Status commit() override {
        inTransaction_ = false;
        return db_->exec("COMMIT");
    }
    Status rollback() override {
        inTransaction_ = false;
        return db_->exec("ROLLBACK");
    }
    Status exec(const std::string& sql) override;

private:
    SqlitePool& pool_;
    std::unique_ptr<Sqlite> db_;
    bool inTransaction_ = false;
};

class SqlitePool : public Database {
public:
    SqlitePool(std::string path, DbLimits limits, size_t size) : path_(std::move(path)), limits_(limits), size_(size) {}

    SqlDialect dialect() const override { return SqlDialect::Sqlite; }

    Result<Datum> query(const NamedQuery& query, const std::vector<Datum>& params) override {
        auto db = take();
        if (!db) return db.error();
        Result<Datum> result = runQuery(*db.value(), query, params, limits_);
        give(std::move(db).value());
        return result;
    }

    Result<std::unique_ptr<DbConnection>> connection() override {
        auto db = take();
        if (!db) return db.error();
        return std::unique_ptr<DbConnection>(new SqliteConnection(*this, std::move(db).value()));
    }

    void closeIdle() override {
        std::vector<std::unique_ptr<Sqlite>> closing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing.swap(idle_);
            open_ -= closing.size();
        }
        cv_.notify_all();
    }

    Result<std::unique_ptr<Sqlite>> take() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !idle_.empty() || open_ < size_; });
        if (!idle_.empty()) {
            auto db = std::move(idle_.back());
            idle_.pop_back();
            return db;
        }
        open_++;
        lock.unlock();
        auto db = Sqlite::open(path_);
        if (!db) {
            std::lock_guard<std::mutex> relock(mutex_);
            open_--;
            cv_.notify_one();
            return makeError(db.error().code, "can't open the database: " + db.error().message);
        }
        return db;
    }

    void give(std::unique_ptr<Sqlite> db) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            idle_.push_back(std::move(db));
        }
        cv_.notify_one();
    }

    // A connection that broke (or is being dropped).
    void discard() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_--;
        }
        cv_.notify_one();
    }

    const DbLimits& limits() const { return limits_; }

private:
    std::string path_;
    DbLimits limits_;
    size_t size_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<Sqlite>> idle_;
    size_t open_ = 0;
};

SqliteConnection::~SqliteConnection() {
    if (inTransaction_ && !db_->exec("ROLLBACK")) {
        db_.reset();   // in an unknown state: close it, rather than reuse it
        pool_.discard();
        return;
    }
    pool_.give(std::move(db_));
}

Result<Datum> SqliteConnection::query(const NamedQuery& query, const std::vector<Datum>& params) {
    return runQuery(*db_, query, params, pool_.limits());
}

Status SqliteConnection::exec(const std::string& sql) {
    db_->setDeadline(std::nullopt);
    return db_->exec(sql);
}

}  // namespace

bool isWholeNumber(double x) { return std::floor(x) == x && std::fabs(x) <= 9007199254740992.0; }

Result<Datum> textValue(const std::string& text, const std::string& column) {
    if (text.find('\0') != std::string::npos) {
        return makeError(ErrorCode::Invalid, "column " + column + " holds text with a NUL character, which "
                                                                  "MiniScript strings can't hold");
    }
    return Datum::string(isValidUtf8(text) ? text : toValidUtf8(text));
}

Error binaryColumn(const std::string& column) {
    return makeError(ErrorCode::Invalid, "column " + column + " holds binary data (a BLOB or bytea), which db.query "
                                                              "can't return; store text instead");
}

Error timedOut(const DbLimits& limits) {
    return makeError(ErrorCode::TooLarge, "the query took longer than its time limit (" +
                                              std::to_string(limits.queryTimeout.count()) + " ms)");
}

Status RowCollector::add(Datum row) {
    if (rows_.size() >= limits_.maxRows) {
        return makeError(ErrorCode::TooLarge, "the query returned more than " + std::to_string(limits_.maxRows) +
                                                  " rows; add a LIMIT");
    }
    bytes_ += toJson(row).size();
    if (bytes_ > limits_.maxBytes) {
        return makeError(ErrorCode::TooLarge, "the query's result is more than " +
                                                  std::to_string(limits_.maxBytes / (1024 * 1024)) + " MB");
    }
    rows_.push_back(std::move(row));
    return {};
}

Result<std::unique_ptr<Database>> Database::openSqlite(const std::string& path, DbLimits limits, size_t poolSize) {
    auto pool = std::make_unique<SqlitePool>(path, limits, poolSize ? poolSize : 1);
    // Open one connection now, so a bad path fails here, not on first use.
    auto first = pool->take();
    if (!first) return first.error();
    pool->give(std::move(first).value());
    return std::unique_ptr<Database>(std::move(pool));
}

namespace {

class LazyDatabase : public Database {
public:
    LazyDatabase(SqlDialect dialect, std::function<Result<std::unique_ptr<Database>>()> open)
        : dialect_(dialect), open_(std::move(open)) {}

    SqlDialect dialect() const override { return dialect_; }

    Result<Datum> query(const NamedQuery& query, const std::vector<Datum>& params) override {
        auto db = get();
        if (!db) return db.error();
        return db.value()->query(query, params);
    }

    Result<std::unique_ptr<DbConnection>> connection() override {
        auto db = get();
        if (!db) return db.error();
        return db.value()->connection();
    }

    void closeIdle() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_) db_->closeIdle();
    }

private:
    Result<Database*> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) {
            auto opened = open_();
            if (!opened) return opened.error();
            db_ = std::move(opened).value();
        }
        return db_.get();
    }

    SqlDialect dialect_;
    std::function<Result<std::unique_ptr<Database>>()> open_;
    std::mutex mutex_;
    std::unique_ptr<Database> db_;
};

}  // namespace

std::unique_ptr<Database> makeLazyDatabase(SqlDialect dialect,
                                           std::function<Result<std::unique_ptr<Database>>()> open) {
    return std::make_unique<LazyDatabase>(dialect, std::move(open));
}

}  // namespace fourfold

// Database over Postgres, through libpq: a pool of connections. Built only
// when libpq was found (FOURFOLD_HAVE_POSTGRES); otherwise openPostgres
// says this build has no Postgres support.

#include "appdata/database.h"

#ifdef FOURFOLD_HAVE_POSTGRES

#include <libpq-fe.h>

#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

#include "db_common.h"

namespace fourfold {

namespace {

// Type OIDs (pg_type.h), for turning text results into numbers and data.
constexpr Oid kBool = 16, kBytea = 17, kInt8 = 20, kInt2 = 21, kInt4 = 23, kOid = 26, kFloat4 = 700,
              kFloat8 = 701, kNumeric = 1700, kJson = 114, kJsonb = 3802;

struct PgResult {
    PGresult* r;
    ~PgResult() { PQclear(r); }
};

std::string trimmed(const char* message) {
    std::string s = message ? message : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

Error resultError(PGresult* r, PGconn* conn, const DbLimits& limits) {
    const char* state = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : nullptr;
    if (state && std::string(state) == "57014") return timedOut(limits);   // query_canceled
    std::string message = trimmed(r ? PQresultErrorMessage(r) : PQerrorMessage(conn));
    if (message.rfind("ERROR:", 0) == 0) message = trimmed(message.c_str() + 6);
    while (!message.empty() && message[0] == ' ') message.erase(0, 1);
    ErrorCode code = ErrorCode::IO;
    if (state && std::string(state).rfind("23", 0) == 0) code = ErrorCode::Conflict;   // integrity constraints
    if (state && std::string(state).rfind("42", 0) == 0) code = ErrorCode::Invalid;    // syntax and names
    return makeError(code, "Postgres: " + message);
}

std::string paramText(const Datum& value) {
    switch (value.kind()) {
        case Datum::Kind::Number: {
            double x = value.asNumber();
            if (isWholeNumber(x)) return std::to_string(static_cast<int64_t>(x));
            return toJson(value);
        }
        case Datum::Kind::String: return value.asString();
        default: return toJson(value);   // a list or map: JSON
    }
}

Result<Datum> columnValue(PGresult* r, int row, int column, const std::string& name) {
    if (PQgetisnull(r, row, column)) return Datum();
    const char* text = PQgetvalue(r, row, column);
    int length = PQgetlength(r, row, column);
    std::string s(text, static_cast<size_t>(length));
    switch (PQftype(r, column)) {
        case kBool: return Datum::number(s == "t" ? 1 : 0);
        case kInt2:
        case kInt4:
        case kInt8:
        case kOid:
        case kFloat4:
        case kFloat8:
        case kNumeric: {
            errno = 0;
            char* end = nullptr;
            double x = std::strtod(s.c_str(), &end);
            if (end && *end == '\0' && std::isfinite(x)) return Datum::number(x);
            return Datum::string(s);   // NaN, Infinity
        }
        case kJson:
        case kJsonb: {
            auto parsed = parseJson(s);
            if (parsed) return std::move(parsed).value();
            return textValue(s, name);
        }
        case kBytea: return binaryColumn(name);
        default: return textValue(s, name);
    }
}

Result<Datum> runQuery(PGconn* conn, const NamedQuery& query, const std::vector<Datum>& params,
                       const DbLimits& limits) {
    std::vector<std::string> texts;
    std::vector<const char*> values;
    texts.reserve(params.size());
    for (const Datum& p : params) texts.push_back(paramText(p));
    for (size_t i = 0; i < params.size(); i++) values.push_back(params[i].isNull() ? nullptr : texts[i].c_str());
    PgResult result{PQexecParams(conn, query.sql.sql.c_str(), static_cast<int>(values.size()), nullptr,
                                 values.empty() ? nullptr : values.data(), nullptr, nullptr, 0)};
    ExecStatusType status = PQresultStatus(result.r);
    if (status == PGRES_COMMAND_OK) return Datum::list({});
    if (status != PGRES_TUPLES_OK) return resultError(result.r, conn, limits);

    int rowCount = PQntuples(result.r), columns = PQnfields(result.r);
    std::vector<std::string> names;
    for (int c = 0; c < columns; c++) names.push_back(PQfname(result.r, c));
    RowCollector rows(limits);
    for (int i = 0; i < rowCount; i++) {
        Datum::MapItems row;
        for (int c = 0; c < columns; c++) {
            auto v = columnValue(result.r, i, c, names[c]);
            if (!v) return v.error();
            row[names[c]] = std::move(v).value();
        }
        if (Status s = rows.add(Datum::map(std::move(row))); !s) return s.error();
    }
    return rows.result();
}

Status runExec(PGconn* conn, const std::string& sql, const DbLimits& limits) {
    PgResult result{PQexec(conn, sql.c_str())};
    ExecStatusType status = PQresultStatus(result.r);
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_EMPTY_QUERY) return {};
    return resultError(result.r, conn, limits);
}

class PgPool;

class PgConnection : public DbConnection {
public:
    PgConnection(PgPool& pool, PGconn* conn) : pool_(pool), conn_(conn) {}
    ~PgConnection() override;

    Result<Datum> query(const NamedQuery& query, const std::vector<Datum>& params) override;
    Status begin() override;
    Status commit() override;
    Status rollback() override;
    Status exec(const std::string& sql) override;

private:
    PgPool& pool_;
    PGconn* conn_;
    bool inTransaction_ = false;
};

class PgPool : public Database {
public:
    PgPool(PostgresSettings settings, DbLimits limits, size_t size)
        : settings_(std::move(settings)), limits_(limits), size_(size) {}
    ~PgPool() override {
        for (PGconn* c : idle_) PQfinish(c);
    }

    SqlDialect dialect() const override { return SqlDialect::Postgres; }

    Result<Datum> query(const NamedQuery& query, const std::vector<Datum>& params) override {
        auto conn = take();
        if (!conn) return conn.error();
        Result<Datum> result = runQuery(conn.value(), query, params, limits_);
        give(conn.value());
        return result;
    }

    Result<std::unique_ptr<DbConnection>> connection() override {
        auto conn = take();
        if (!conn) return conn.error();
        return std::unique_ptr<DbConnection>(new PgConnection(*this, conn.value()));
    }

    void closeIdle() override {
        std::vector<PGconn*> closing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing.swap(idle_);
            open_ -= closing.size();
        }
        for (PGconn* c : closing) PQfinish(c);
        cv_.notify_all();
    }

    Result<PGconn*> take() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !idle_.empty() || open_ < size_; });
        if (!idle_.empty()) {
            PGconn* c = idle_.back();
            idle_.pop_back();
            return c;
        }
        open_++;
        lock.unlock();
        auto c = connect();
        if (!c) {
            std::lock_guard<std::mutex> relock(mutex_);
            open_--;
            cv_.notify_one();
        }
        return c;
    }

    // Back to the pool, or closed if it broke.
    void give(PGconn* conn) {
        if (PQstatus(conn) != CONNECTION_OK || PQtransactionStatus(conn) != PQTRANS_IDLE) {
            PQfinish(conn);
            std::lock_guard<std::mutex> lock(mutex_);
            open_--;
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            idle_.push_back(conn);
        }
        cv_.notify_one();
    }

    const DbLimits& limits() const { return limits_; }

private:
    Result<PGconn*> connect() {
        std::string options = "-c statement_timeout=" + std::to_string(limits_.queryTimeout.count());
        if (!settings_.schema.empty()) options += " -c search_path=" + settings_.schema;
        const char* keys[] = {"dbname", "password", "options", "client_encoding", "connect_timeout", nullptr};
        const char* values[] = {settings_.url.c_str(), settings_.password.empty() ? nullptr : settings_.password.c_str(),
                                options.c_str(), "UTF8", "10", nullptr};
        if (settings_.password.empty()) {
            keys[1] = "options";
            values[1] = options.c_str();
            keys[2] = "client_encoding";
            values[2] = "UTF8";
            keys[3] = "connect_timeout";
            values[3] = "10";
            keys[4] = nullptr;
            values[4] = nullptr;
        }
        PGconn* conn = PQconnectdbParams(keys, values, 1);
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            std::string message = conn ? trimmed(PQerrorMessage(conn)) : "out of memory";
            if (conn) PQfinish(conn);
            return makeError(ErrorCode::IO, "can't connect to Postgres: " + message);
        }
        // Notices (e.g. "table already exists, skipping") aren't errors.
        PQsetNoticeProcessor(conn, [](void*, const char*) {}, nullptr);
        return conn;
    }

    PostgresSettings settings_;
    DbLimits limits_;
    size_t size_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<PGconn*> idle_;
    size_t open_ = 0;
};

PgConnection::~PgConnection() {
    if (inTransaction_) (void)runExec(conn_, "ROLLBACK", pool_.limits());
    pool_.give(conn_);
}

Result<Datum> PgConnection::query(const NamedQuery& query, const std::vector<Datum>& params) {
    return runQuery(conn_, query, params, pool_.limits());
}

Status PgConnection::begin() {
    if (Status s = runExec(conn_, "BEGIN", pool_.limits()); !s) return s;
    inTransaction_ = true;
    return {};
}

Status PgConnection::commit() {
    inTransaction_ = false;
    return runExec(conn_, "COMMIT", pool_.limits());
}

Status PgConnection::rollback() {
    inTransaction_ = false;
    return runExec(conn_, "ROLLBACK", pool_.limits());
}

Status PgConnection::exec(const std::string& sql) {
    // A migration may run long: no statement timeout for it.
    (void)runExec(conn_, "SET LOCAL statement_timeout = 0", pool_.limits());
    return runExec(conn_, sql, pool_.limits());
}

}  // namespace

bool Database::postgresSupported() { return true; }

Result<std::unique_ptr<Database>> Database::openPostgres(const PostgresSettings& settings, DbLimits limits,
                                                         size_t poolSize) {
    auto pool = std::make_unique<PgPool>(settings, limits, poolSize ? poolSize : 1);
    auto first = pool->take();
    if (!first) return first.error();
    pool->give(first.value());
    return std::unique_ptr<Database>(std::move(pool));
}

}  // namespace fourfold

#else

namespace fourfold {

bool Database::postgresSupported() { return false; }

Result<std::unique_ptr<Database>> Database::openPostgres(const PostgresSettings&, DbLimits, size_t) {
    return makeError(ErrorCode::Invalid, "this build of Fourfold has no Postgres support (it was built without libpq)");
}

}  // namespace fourfold

#endif

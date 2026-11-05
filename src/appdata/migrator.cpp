#include "appdata/migrator.h"

#include <algorithm>
#include <ctime>
#include <set>

namespace fourfold {

namespace {

const char* const kDataRecord = "migrations.json";
constexpr size_t kMaxRecordBytes = 4 * 1024 * 1024;

const char* const kCreateTable =
    "CREATE TABLE IF NOT EXISTS fourfold_migrations (number INTEGER PRIMARY KEY, name TEXT NOT NULL, "
    "file TEXT NOT NULL, checksum TEXT NOT NULL, applied_at TEXT NOT NULL)";

NamedQuery engineQuery(SqlDialect dialect, const std::string& sql) {
    std::vector<SqlProblem> problems;
    NamedQuery q;
    q.name = "(fourfold)";
    q.sql = prepareSql(sql, dialect, problems);
    return q;
}

std::string isoTime(int64_t millis) {
    std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string str(const Datum::MapItems& row, const char* key) {
    auto it = row.find(key);
    return it != row.end() && it->second.kind() == Datum::Kind::String ? it->second.asString() : "";
}

int num(const Datum::MapItems& row, const char* key) {
    auto it = row.find(key);
    return it != row.end() && it->second.kind() == Datum::Kind::Number ? static_cast<int>(it->second.asNumber()) : 0;
}

std::string label(int number, const std::string& name) {
    return name.empty() ? "migration " + std::to_string(number) : name;
}

}  // namespace

MigrationPlan planMigrations(const CompiledApp& app, const AppliedMigrations& applied, bool allowOlderCode) {
    MigrationPlan plan;
    std::map<int, const Migration*> known;
    for (const Migration& m : app.migrations) known[m.number] = &m;
    int lastKnown = app.migrations.empty() ? 0 : app.migrations.back().number;
    int lastApplied = applied.empty() ? 0 : applied.rbegin()->first;

    for (const auto& [number, record] : applied) {
        auto it = known.find(number);
        if (it == known.end()) {
            if (number > lastKnown) {
                std::string message = record.name + " was applied to this data, but this code doesn't have it: is "
                                                    "it older code? Restore a backup from before " +
                                      record.name + ", or deploy code that includes it";
                (allowOlderCode ? plan.warnings : plan.errors).push_back(message);
            } else {
                plan.errors.push_back(record.name + " was applied (" + record.appliedAt +
                                      "), but code/migrations/ no longer has it; put it back");
            }
            continue;
        }
        const Migration& m = *it->second;
        if (m.checksum != record.checksum) {
            plan.errors.push_back("code/migrations/" + m.fileLabel() + " was changed after it was applied (" +
                                  record.appliedAt + "); write a new migration instead");
        }
    }
    for (const Migration& m : app.migrations) {
        if (applied.count(m.number)) continue;
        if (m.number < lastApplied) {
            plan.errors.push_back("code/migrations/" + m.fileLabel() + " is numbered below " +
                                  applied.rbegin()->second.name +
                                  ", which is already applied; renumber it to come after");
            continue;
        }
        plan.pending.push_back(&m);
    }
    return plan;
}

Migrator::Migrator(Database* db, const DataDir& data, ScriptRunner scripts, const Clock& clock)
    : db_(db), data_(data), scripts_(std::move(scripts)), clock_(clock) {}

Status Migrator::inject(const std::string& step) const { return inject_ ? inject_(step) : Status{}; }

Status Migrator::ensureTable() const {
    if (!db_) return {};
    auto conn = db_->connection();
    if (!conn) return conn.error();
    return conn.value()->exec(kCreateTable);
}

Result<AppliedMigrations> Migrator::applied(DbConnection* tx) const {
    AppliedMigrations out;
    if (db_ && (tx || !dbExists_ || dbExists_())) {
        if (!tx) {
            if (Status s = ensureTable(); !s) return s.error();
        }
        QueryRunner& runner = tx ? static_cast<QueryRunner&>(*tx) : *db_;
        auto rows = runner.query(
            engineQuery(db_->dialect(), "SELECT number, name, file, checksum, applied_at FROM fourfold_migrations"), {});
        if (!rows) return rows.error();
        for (const Datum& row : rows.value().asList()) {
            const Datum::MapItems& r = row.asMap();
            AppliedMigration a;
            a.number = num(r, "number");
            a.name = str(r, "name");
            a.file = str(r, "file");
            a.checksum = str(r, "checksum");
            a.appliedAt = str(r, "applied_at");
            out[a.number] = a;
        }
    }
    auto file = data_.readEngineFile(kDataRecord, kMaxRecordBytes);
    if (!file) return file.error();
    if (file.value()) {
        auto parsed = parseJson(*file.value());
        if (!parsed || parsed.value().kind() != Datum::Kind::List) {
            return makeError(ErrorCode::Invalid, "data/.fourfold/migrations.json is damaged");
        }
        for (const Datum& item : parsed.value().asList()) {
            if (item.kind() != Datum::Kind::Map) continue;
            const Datum::MapItems& r = item.asMap();
            AppliedMigration a;
            a.number = num(r, "number");
            a.name = str(r, "name");
            a.file = str(r, "file");
            a.checksum = str(r, "checksum");
            a.appliedAt = str(r, "appliedAt");
            a.inDatabase = false;
            out[a.number] = a;
        }
    }
    return out;
}

Status Migrator::writeDataRecord(const AppliedMigrations& records) const {
    Datum::ListItems items;
    for (const auto& [number, a] : records) {
        if (a.inDatabase) continue;
        items.push_back(Datum::map({{"number", Datum::number(a.number)},
                                    {"name", Datum::string(a.name)},
                                    {"file", Datum::string(a.file)},
                                    {"checksum", Datum::string(a.checksum)},
                                    {"appliedAt", Datum::string(a.appliedAt)}}));
    }
    return data_.writeEngineFile(kDataRecord, toJson(Datum::list(std::move(items))) + "\n");
}

AppliedMigration Migrator::recordFor(const Migration& m) const {
    AppliedMigration a;
    a.number = m.number;
    a.name = m.name;
    a.file = m.kind == Migration::Kind::Folder ? m.fileLabel() : std::string(m.source.name());
    a.checksum = m.checksum;
    a.appliedAt = isoTime(clock_.nowMillis());
    a.inDatabase = m.usesDatabase();
    return a;
}

Status Migrator::applyOne(const CompiledApp& app, const Migration& m, DbConnection* tx) {
    if (Status s = inject("before " + std::to_string(m.number)); !s) return s;
    AppliedMigration record = recordFor(m);

    if (!m.usesDatabase()) {
        if (Status s = scripts_(app, m, nullptr); !s) return s;
        if (Status s = inject("after " + std::to_string(m.number)); !s) return s;
        auto records = applied(tx);
        if (!records) return records.error();
        records.value()[m.number] = record;
        return writeDataRecord(records.value());
    }

    if (!tx) return makeError(ErrorCode::Invalid, "the app has no database to migrate");
    SqlDialect d = db_->dialect();
    if (m.kind == Migration::Kind::Sql) {
        if (Status s = tx->exec(m.sql); !s) return s;
    } else {
        if (Status s = scripts_(app, m, tx); !s) return s;
    }
    if (Status s = inject("after " + std::to_string(m.number)); !s) return s;
    auto inserted = tx->query(engineQuery(d, "INSERT INTO fourfold_migrations (number, name, file, checksum, "
                                             "applied_at) VALUES ({{0}}, {{1}}, {{2}}, {{3}}, {{4}})"),
                              {Datum::number(record.number), Datum::string(record.name), Datum::string(record.file),
                               Datum::string(record.checksum), Datum::string(record.appliedAt)});
    if (!inserted) return inserted.error();
    return {};
}

Migrator::Outcome Migrator::apply(const CompiledApp& app, const std::vector<const Migration*>& pending) {
    Outcome out;
    auto failed = [&](const Migration* m, Error e) {
        if (m) e.message = label(m->number, m->name) + ": " + e.message;
        out.error = std::move(e);
        out.failed = m ? m->number : 0;
        out.applied.clear();   // the transaction is rolled back with it
        return out;
    };

    // The whole batch's database work is one transaction: if any migration
    // fails, none of the batch stays in the database.
    std::unique_ptr<DbConnection> tx;
    bool needsDb = false;
    for (const Migration* m : pending) needsDb = needsDb || m->usesDatabase();
    if (needsDb) {
        if (!db_) return failed(nullptr, makeError(ErrorCode::Invalid, "the app has no database to migrate"));
        if (Status s = ensureTable(); !s) return failed(nullptr, s.error());
        auto conn = db_->connection();
        if (!conn) return failed(nullptr, conn.error());
        tx = std::move(conn).value();
        if (Status s = tx->begin(); !s) return failed(nullptr, s.error());
        if (db_->dialect() == SqlDialect::Postgres) {
            // Several servers sharing the database: one migrates, the others
            // wait here, then find the migrations already done.
            auto locked = tx->query(engineQuery(SqlDialect::Postgres, "SELECT pg_advisory_xact_lock(4442001) AS l"), {});
            if (!locked) return failed(nullptr, locked.error());
        }
    }
    auto alreadyDone = needsDb ? applied(tx.get()) : applied();
    if (!alreadyDone) return failed(nullptr, alreadyDone.error());

    for (const Migration* m : pending) {
        if (alreadyDone.value().count(m->number)) continue;   // another server did it
        if (Status s = applyOne(app, *m, tx.get()); !s) return failed(m, s.error());
        out.applied.push_back(m->number);
    }
    if (tx) {
        if (Status s = inject("commit"); !s) return failed(nullptr, s.error());
        if (Status s = tx->commit(); !s) return failed(nullptr, s.error());
    }
    return out;
}

Status Migrator::forget(int number) {
    auto records = applied();
    if (!records) return records.error();
    auto it = records.value().find(number);
    if (it == records.value().end()) return {};
    if (!it->second.inDatabase) {
        records.value().erase(it);
        return writeDataRecord(records.value());
    }
    if (!db_) return {};
    auto removed = db_->query(engineQuery(db_->dialect(), "DELETE FROM fourfold_migrations WHERE number = {{0}}"),
                              {Datum::number(number)});
    if (!removed) return removed.error();
    return {};
}

}  // namespace fourfold

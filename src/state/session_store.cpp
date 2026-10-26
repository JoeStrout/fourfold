#include "state/session_store.h"

#include "base/crypto.h"
#include "platform/random.h"

namespace fourfold {

namespace {

constexpr size_t kIdBytes = 32;

const char* const kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS sessions (
    id_hash BLOB PRIMARY KEY,
    user_id TEXT,
    created_at INTEGER NOT NULL,
    last_used_at INTEGER NOT NULL,
    address_group TEXT NOT NULL DEFAULT '',
    engine BLOB NOT NULL,
    app BLOB NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS sessions_user ON sessions (user_id) WHERE user_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS sessions_anonymous ON sessions (last_used_at) WHERE user_id IS NULL;
CREATE INDEX IF NOT EXISTS sessions_anonymous_address
    ON sessions (address_group, last_used_at) WHERE user_id IS NULL;
)SQL";

}  // namespace

Result<std::unique_ptr<SessionStore>> SessionStore::open(const std::string& path, SessionConfig config,
                                                         const Clock& clock) {
    auto db = Sqlite::open(path);
    if (!db) return db.error();
    if (Status s = db.value()->exec(kSchema); !s) return s.error();
    return std::unique_ptr<SessionStore>(new SessionStore(std::move(db).value(), std::move(config), clock));
}

std::string SessionStore::newId() { return toBase64Url(randomBytes(kIdBytes)); }

std::optional<std::string> SessionStore::hashOfId(std::string_view cookieValue) {
    if (cookieValue.size() != (kIdBytes * 4 + 2) / 3) return std::nullopt;
    auto raw = fromBase64Url(cookieValue);
    if (!raw || raw->size() != kIdBytes) return std::nullopt;
    return hashBytes(*raw);
}

bool SessionStore::expired(const SessionRecord& r, int64_t now) const {
    bool signedIn = r.userId.has_value();
    auto idle = signedIn ? config_.signedInIdle : config_.anonymousIdle;
    auto limit = signedIn ? config_.signedInLimit : config_.anonymousLimit;
    return now - r.lastUsedAt > idle.count() || now - r.createdAt > limit.count();
}

Result<std::optional<SessionRecord>> SessionStore::load(const std::string& idHash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement(
        "SELECT user_id, created_at, last_used_at, engine, app FROM sessions WHERE id_hash = ?1");
    if (!stmt) return stmt.error();
    SqliteStatement& s = *stmt.value();
    s.bindBlob(1, idHash);
    auto row = s.step();
    if (!row) return row.error();
    if (!row.value()) return std::optional<SessionRecord>();
    SessionRecord r;
    r.idHash = idHash;
    if (!s.isNull(0)) r.userId = s.text(0);
    r.createdAt = s.integer(1);
    r.lastUsedAt = s.integer(2);
    r.engine = s.text(3);
    r.app = s.text(4);
    if (Status done = s.run(); !done) return done.error();
    if (expired(r, clock_.nowMillis())) {
        auto del = db_->statement("DELETE FROM sessions WHERE id_hash = ?1");
        if (!del) return del.error();
        del.value()->bindBlob(1, idHash);
        if (Status d = del.value()->run(); !d) return d.error();
        return std::optional<SessionRecord>();
    }
    return std::optional<SessionRecord>(std::move(r));
}

Status SessionStore::evictAnonymous(const std::string& addressGroup) {
    // Make room for one more: at most max - 1 may remain, per group and in all.
    auto perGroup = db_->statement(
        "DELETE FROM sessions WHERE id_hash IN ("
        " SELECT id_hash FROM sessions WHERE user_id IS NULL AND address_group = ?1"
        " ORDER BY last_used_at DESC LIMIT -1 OFFSET ?2)");
    if (!perGroup) return perGroup.error();
    perGroup.value()->bindText(1, addressGroup).bindInt(2, static_cast<int64_t>(config_.maxAnonymousPerAddress) - 1);
    if (Status s = perGroup.value()->run(); !s) return s;
    auto total = db_->statement(
        "DELETE FROM sessions WHERE id_hash IN ("
        " SELECT id_hash FROM sessions WHERE user_id IS NULL"
        " ORDER BY last_used_at DESC LIMIT -1 OFFSET ?1)");
    if (!total) return total.error();
    total.value()->bindInt(1, static_cast<int64_t>(config_.maxAnonymous) - 1);
    return total.value()->run();
}

Status SessionStore::create(const std::string& id, const std::string& addressGroup, const std::string& engine,
                            const std::string& app) {
    auto hash = hashOfId(id);
    if (!hash) return makeError(ErrorCode::Invalid, "not a session ID");
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->transaction([&]() -> Status {
        if (Status s = evictAnonymous(addressGroup); !s) return s;
        auto stmt = db_->statement(
            "INSERT INTO sessions (id_hash, user_id, created_at, last_used_at, address_group, engine, app)"
            " VALUES (?1, NULL, ?2, ?2, ?3, ?4, ?5)");
        if (!stmt) return stmt.error();
        int64_t now = clock_.nowMillis();
        stmt.value()->bindBlob(1, *hash).bindInt(2, now).bindText(3, addressGroup).bindBlob(4, engine).bindBlob(5, app);
        return stmt.value()->run();
    });
}

Status SessionStore::save(const std::string& idHash, const std::string& engine, const std::string& app) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("UPDATE sessions SET engine = ?2, app = ?3, last_used_at = ?4 WHERE id_hash = ?1");
    if (!stmt) return stmt.error();
    stmt.value()->bindBlob(1, idHash).bindBlob(2, engine).bindBlob(3, app).bindInt(4, clock_.nowMillis());
    if (Status s = stmt.value()->run(); !s) return s;
    if (db_->changes() == 0) return makeError(ErrorCode::NotFound, "the session has ended");
    return {};
}

Result<bool> SessionStore::replaceEngine(const std::string& idHash, const std::string& expected,
                                         const std::string& engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("UPDATE sessions SET engine = ?3 WHERE id_hash = ?1 AND engine = ?2");
    if (!stmt) return stmt.error();
    stmt.value()->bindBlob(1, idHash).bindBlob(2, expected).bindBlob(3, engine);
    if (Status s = stmt.value()->run(); !s) return s.error();
    return db_->changes() > 0;
}

Status SessionStore::touch(const std::string& idHash, int64_t lastUsedAt) {
    int64_t now = clock_.nowMillis();
    if (now - lastUsedAt < config_.touchInterval.count()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("UPDATE sessions SET last_used_at = ?2 WHERE id_hash = ?1");
    if (!stmt) return stmt.error();
    stmt.value()->bindBlob(1, idHash).bindInt(2, now);
    return stmt.value()->run();
}

Status SessionStore::remove(const std::string& idHash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("DELETE FROM sessions WHERE id_hash = ?1");
    if (!stmt) return stmt.error();
    stmt.value()->bindBlob(1, idHash);
    return stmt.value()->run();
}

Status SessionStore::removeForUser(const std::string& userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("DELETE FROM sessions WHERE user_id = ?1");
    if (!stmt) return stmt.error();
    stmt.value()->bindText(1, userId);
    return stmt.value()->run();
}

Result<int64_t> SessionStore::sweepExpired() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = clock_.nowMillis();
    auto stmt = db_->statement(
        "DELETE FROM sessions WHERE"
        " (user_id IS NULL AND (last_used_at < ?1 OR created_at < ?2)) OR"
        " (user_id IS NOT NULL AND (last_used_at < ?3 OR created_at < ?4))");
    if (!stmt) return stmt.error();
    stmt.value()
        ->bindInt(1, now - config_.anonymousIdle.count())
        .bindInt(2, now - config_.anonymousLimit.count())
        .bindInt(3, now - config_.signedInIdle.count())
        .bindInt(4, now - config_.signedInLimit.count());
    if (Status s = stmt.value()->run(); !s) return s.error();
    return db_->changes();
}

Result<int64_t> SessionStore::count() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("SELECT count(*) FROM sessions");
    if (!stmt) return stmt.error();
    auto row = stmt.value()->step();
    if (!row) return row.error();
    int64_t n = stmt.value()->integer(0);
    if (Status s = stmt.value()->run(); !s) return s.error();
    return n;
}

}  // namespace fourfold

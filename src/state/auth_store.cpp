#include "state/auth_store.h"

#include <algorithm>
#include <cctype>
#include <tuple>

#include "base/crypto.h"
#include "platform/random.h"

namespace fourfold {

namespace {

constexpr size_t kUserIdBytes = 16;
constexpr size_t kTokenBytes = 32;

const char* const kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id TEXT PRIMARY KEY,
    email TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    created_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS user_roles (
    user_id TEXT NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    role TEXT NOT NULL,
    PRIMARY KEY (user_id, role)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS login_tokens (
    token_hash BLOB PRIMARY KEY,
    email TEXT NOT NULL,
    address_group TEXT NOT NULL,
    next TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    used_at INTEGER
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS login_tokens_email ON login_tokens (email, created_at);
CREATE INDEX IF NOT EXISTS login_tokens_address ON login_tokens (address_group, created_at);
)SQL";

}  // namespace

Result<std::unique_ptr<AuthStore>> AuthStore::open(const std::string& path, AuthStoreConfig config,
                                                   const Clock& clock) {
    auto db = Sqlite::open(path);
    if (!db) return db.error();
    if (Status s = db.value()->exec(kSchema); !s) return s.error();
    return std::unique_ptr<AuthStore>(new AuthStore(std::move(db).value(), config, clock));
}

void AuthStore::setConfig(AuthStoreConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

std::string AuthStore::normalizeEmail(std::string_view email) {
    while (!email.empty() && std::isspace(static_cast<unsigned char>(email.front()))) email.remove_prefix(1);
    while (!email.empty() && std::isspace(static_cast<unsigned char>(email.back()))) email.remove_suffix(1);
    std::string out(email);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

Result<std::optional<AuthUser>> AuthStore::loadWhere(const char* sql, const std::string& key) {
    auto stmt = db_->statement(sql);
    if (!stmt) return stmt.error();
    SqliteStatement& s = *stmt.value();
    s.bindText(1, key);
    auto row = s.step();
    if (!row) return row.error();
    if (!row.value()) return std::optional<AuthUser>();
    AuthUser u;
    u.id = s.text(0);
    u.email = s.text(1);
    u.name = s.text(2);
    u.createdAt = s.integer(3);
    if (Status done = s.run(); !done) return done.error();

    auto roles = db_->statement("SELECT role FROM user_roles WHERE user_id = ?1 ORDER BY role");
    if (!roles) return roles.error();
    roles.value()->bindText(1, u.id);
    for (;;) {
        auto r = roles.value()->step();
        if (!r) return r.error();
        if (!r.value()) break;
        u.roles.push_back(roles.value()->text(0));
    }
    return std::optional<AuthUser>(std::move(u));
}

Result<std::optional<AuthUser>> AuthStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadWhere("SELECT id, email, name, created_at FROM users WHERE id = ?1", id);
}

Result<std::optional<AuthUser>> AuthStore::findByEmail(std::string_view email) {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadWhere("SELECT id, email, name, created_at FROM users WHERE email = ?1", normalizeEmail(email));
}

Result<AuthUser> AuthStore::create(std::string_view email, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    AuthUser u;
    u.id = toBase64Url(randomBytes(kUserIdBytes));
    u.email = normalizeEmail(email);
    u.name = name;
    u.createdAt = clock_.nowMillis();
    auto existing = loadWhere("SELECT id, email, name, created_at FROM users WHERE email = ?1", u.email);
    if (!existing) return existing.error();
    if (existing.value()) return makeError(ErrorCode::Conflict, "there's already a user with that email address");
    auto stmt = db_->statement("INSERT INTO users (id, email, name, created_at) VALUES (?1, ?2, ?3, ?4)");
    if (!stmt) return stmt.error();
    stmt.value()->bindText(1, u.id).bindText(2, u.email).bindText(3, u.name).bindInt(4, u.createdAt);
    if (Status s = stmt.value()->run(); !s) return s.error();
    return u;
}

Status AuthStore::userExists(const std::string& id) {
    auto stmt = db_->statement("SELECT 1 FROM users WHERE id = ?1");
    if (!stmt) return stmt.error();
    stmt.value()->bindText(1, id);
    auto row = stmt.value()->step();
    if (!row) return row.error();
    bool found = row.value();
    if (Status s = stmt.value()->run(); !s) return s;
    if (!found) return makeError(ErrorCode::NotFound, "no such user");
    return {};
}

Status AuthStore::grant(const std::string& id, const std::string& role) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Status s = userExists(id); !s) return s;
    auto stmt = db_->statement("INSERT OR IGNORE INTO user_roles (user_id, role) VALUES (?1, ?2)");
    if (!stmt) return stmt.error();
    stmt.value()->bindText(1, id).bindText(2, role);
    return stmt.value()->run();
}

Status AuthStore::revoke(const std::string& id, const std::string& role) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Status s = userExists(id); !s) return s;
    auto stmt = db_->statement("DELETE FROM user_roles WHERE user_id = ?1 AND role = ?2");
    if (!stmt) return stmt.error();
    stmt.value()->bindText(1, id).bindText(2, role);
    return stmt.value()->run();
}

Status AuthStore::setName(const std::string& id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("UPDATE users SET name = ?2 WHERE id = ?1");
    if (!stmt) return stmt.error();
    stmt.value()->bindText(1, id).bindText(2, name);
    if (Status s = stmt.value()->run(); !s) return s;
    if (db_->changes() == 0) return makeError(ErrorCode::NotFound, "no such user");
    return {};
}

Result<std::vector<AuthUser>> AuthStore::all() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("SELECT id FROM users ORDER BY email");
    if (!stmt) return stmt.error();
    std::vector<std::string> ids;
    for (;;) {
        auto row = stmt.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        ids.push_back(stmt.value()->text(0));
    }
    std::vector<AuthUser> users;
    for (const std::string& id : ids) {
        auto u = loadWhere("SELECT id, email, name, created_at FROM users WHERE id = ?1", id);
        if (!u) return u.error();
        if (u.value()) users.push_back(std::move(*u.value()));
    }
    return users;
}

Result<std::optional<std::string>> AuthStore::createToken(std::string_view email, const std::string& addressGroup,
                                                          const std::string& next) {
    std::string normalized = normalizeEmail(email);
    std::string token = toBase64Url(randomBytes(kTokenBytes));
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = clock_.nowMillis();
    int64_t since = now - config_.throttleWindow.count();
    bool allowed = false;
    Status s = db_->transaction([&]() -> Status {
        for (auto [sql, key, max] : {std::make_tuple("SELECT count(*) FROM login_tokens WHERE email = ?1 AND created_at > ?2",
                                                     normalized, config_.maxTokensPerEmail),
                                     std::make_tuple("SELECT count(*) FROM login_tokens WHERE address_group = ?1 AND "
                                                     "created_at > ?2",
                                                     addressGroup, config_.maxTokensPerAddress)}) {
            auto count = db_->statement(sql);
            if (!count) return count.error();
            count.value()->bindText(1, key).bindInt(2, since);
            auto row = count.value()->step();
            if (!row) return row.error();
            int64_t n = count.value()->integer(0);
            if (Status done = count.value()->run(); !done) return done;
            if (n >= max) return {};
        }
        auto insert = db_->statement(
            "INSERT INTO login_tokens (token_hash, email, address_group, next, created_at, expires_at)"
            " VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
        if (!insert) return insert.error();
        insert.value()
            ->bindBlob(1, hashBytes(token))
            .bindText(2, normalized)
            .bindText(3, addressGroup)
            .bindText(4, next)
            .bindInt(5, now)
            .bindInt(6, now + config_.tokenLifetime.count());
        if (Status done = insert.value()->run(); !done) return done;
        allowed = true;
        return {};
    });
    if (!s) return s.error();
    if (!allowed) return std::optional<std::string>();
    return std::optional<std::string>(token);
}

Result<std::optional<LoginToken>> AuthStore::redeemToken(const std::string& token) {
    auto raw = fromBase64Url(token);
    if (!raw || raw->size() != kTokenBytes) return std::optional<LoginToken>();
    std::string hash = hashBytes(token);
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<LoginToken> out;
    Status s = db_->transaction([&]() -> Status {
        int64_t now = clock_.nowMillis();
        auto use = db_->statement(
            "UPDATE login_tokens SET used_at = ?2 WHERE token_hash = ?1 AND used_at IS NULL AND expires_at > ?2");
        if (!use) return use.error();
        use.value()->bindBlob(1, hash).bindInt(2, now);
        if (Status done = use.value()->run(); !done) return done;
        if (db_->changes() == 0) return {};
        auto read = db_->statement("SELECT email, next FROM login_tokens WHERE token_hash = ?1");
        if (!read) return read.error();
        read.value()->bindBlob(1, hash);
        auto row = read.value()->step();
        if (!row) return row.error();
        if (row.value()) out = LoginToken{read.value()->text(0), read.value()->text(1)};
        return read.value()->run();
    });
    if (!s) return s.error();
    return out;
}

Result<int64_t> AuthStore::sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = clock_.nowMillis();
    auto stmt = db_->statement("DELETE FROM login_tokens WHERE expires_at < ?1 AND created_at < ?2");
    if (!stmt) return stmt.error();
    stmt.value()->bindInt(1, now).bindInt(2, now - config_.throttleWindow.count());
    if (Status s = stmt.value()->run(); !s) return s.error();
    return db_->changes();
}

}  // namespace fourfold

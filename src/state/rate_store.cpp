#include "state/rate_store.h"

#include <algorithm>
#include <cmath>

#include "base/crypto.h"

namespace fourfold {

namespace {

// A window's start, as a count of whole windows since the epoch.
const char* const kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS rate_counts (
    key TEXT NOT NULL,
    seconds INTEGER NOT NULL,
    win INTEGER NOT NULL,
    count INTEGER NOT NULL,
    PRIMARY KEY (key, seconds, win)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS used_challenges (
    id BLOB PRIMARY KEY,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS used_challenges_expiry ON used_challenges (expires_at);
)SQL";

// Long keys (an app's own, from ratelimit()) are stored by their hash.
std::string storedKey(const std::string& key) {
    return key.size() <= 200 ? key : "#" + toHex(hashBytes(key));
}

}  // namespace

Result<std::unique_ptr<RateStore>> RateStore::open(const std::string& path, const Clock& clock) {
    auto db = Sqlite::open(path);
    if (!db) return db.error();
    if (Status s = db.value()->exec(kSchema); !s) return s.error();
    return std::unique_ptr<RateStore>(new RateStore(std::move(db).value(), clock));
}

Result<RateResult> RateStore::hit(const std::vector<RateCheck>& checks) {
    std::lock_guard<std::mutex> lock(mutex_);
    RateResult result;
    int64_t now = clock_.nowMillis();
    Status s = db_->transaction([&]() -> Status {
        result = RateResult();
        struct Pending {
            std::string key;
            int64_t seconds, window, cost;
        };
        std::vector<Pending> pending;
        for (size_t i = 0; i < checks.size(); i++) {
            const RateCheck& c = checks[i];
            int64_t windowMs = std::max<int64_t>(c.seconds, 1) * 1000;
            int64_t window = now / windowMs;
            double into = static_cast<double>(now % windowMs) / static_cast<double>(windowMs);
            std::string key = storedKey(c.key);
            auto stmt = db_->statement(
                "SELECT win, count FROM rate_counts WHERE key = ?1 AND seconds = ?2 AND win >= ?3");
            if (!stmt) return stmt.error();
            stmt.value()->bindText(1, key).bindInt(2, c.seconds).bindInt(3, window - 1);
            int64_t current = 0, previous = 0;
            for (;;) {
                auto row = stmt.value()->step();
                if (!row) return row.error();
                if (!row.value()) break;
                (stmt.value()->integer(0) == window ? current : previous) = stmt.value()->integer(1);
            }
            double estimate = static_cast<double>(previous) * (1 - into) + static_cast<double>(current);
            double cost = static_cast<double>(std::max<int64_t>(c.cost, 1));
            if (estimate + cost > static_cast<double>(c.count) + 1e-9) {
                result.allowed = false;
                result.refusedIndex = i;
                // Until the estimate falls below the limit: at the latest,
                // when this window ends and its count becomes the previous one.
                double wait = (1 - into) * static_cast<double>(c.seconds);
                if (previous > 0 && current < c.count) {
                    // The previous window's share shrinks as time passes.
                    double needed = (estimate + cost - static_cast<double>(c.count)) / static_cast<double>(previous);
                    wait = std::min(wait, needed * static_cast<double>(c.seconds));
                }
                result.retryAfterSeconds = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(wait)));
                result.counts.push_back(estimate);
                return {};   // nothing is counted
            }
            result.counts.push_back(estimate + cost);
            pending.push_back({key, c.seconds, window, static_cast<int64_t>(cost)});
        }
        for (const Pending& p : pending) {
            auto stmt = db_->statement(
                "INSERT INTO rate_counts (key, seconds, win, count) VALUES (?1, ?2, ?3, ?4) "
                "ON CONFLICT (key, seconds, win) DO UPDATE SET count = count + ?4");
            if (!stmt) return stmt.error();
            stmt.value()->bindText(1, p.key).bindInt(2, p.seconds).bindInt(3, p.window).bindInt(4, p.cost);
            if (Status r = stmt.value()->run(); !r) return r;
        }
        return {};
    });
    if (!s) return s.error();
    return result;
}

Result<bool> RateStore::useOnce(const std::string& id, int64_t expiresAtMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stmt = db_->statement("INSERT OR IGNORE INTO used_challenges (id, expires_at) VALUES (?1, ?2)");
    if (!stmt) return stmt.error();
    stmt.value()->bindBlob(1, hashBytes(id)).bindInt(2, expiresAtMs);
    if (Status s = stmt.value()->run(); !s) return s.error();
    return db_->changes() == 1;
}

Result<int64_t> RateStore::sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = clock_.nowMillis();
    int64_t removed = 0;
    // A counter matters while its window or the next one is current.
    auto counts = db_->statement("DELETE FROM rate_counts WHERE (win + 2) * seconds * 1000 <= ?1");
    if (!counts) return counts.error();
    counts.value()->bindInt(1, now);
    if (Status s = counts.value()->run(); !s) return s.error();
    removed += db_->changes();
    auto used = db_->statement("DELETE FROM used_challenges WHERE expires_at <= ?1");
    if (!used) return used.error();
    used.value()->bindInt(1, now);
    if (Status s = used.value()->run(); !s) return s.error();
    removed += db_->changes();
    return removed;
}

Status RateStore::resetCounters() {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->exec("DELETE FROM rate_counts");
}

Result<std::vector<RateStore::Counter>> RateStore::counters() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = clock_.nowMillis();
    auto stmt = db_->statement("SELECT key, seconds, win, count FROM rate_counts ORDER BY key, seconds, win");
    if (!stmt) return stmt.error();
    std::vector<Counter> out;
    for (;;) {
        auto row = stmt.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        std::string key = stmt.value()->text(0);
        int64_t seconds = stmt.value()->integer(1), window = stmt.value()->integer(2), count = stmt.value()->integer(3);
        int64_t windowMs = std::max<int64_t>(seconds, 1) * 1000;
        int64_t nowWindow = now / windowMs;
        double into = static_cast<double>(now % windowMs) / static_cast<double>(windowMs);
        double share = window == nowWindow ? 1 : window == nowWindow - 1 ? 1 - into : 0;
        if (share <= 0) continue;
        if (out.empty() || out.back().key != key || out.back().seconds != seconds) out.push_back({key, seconds, 0});
        out.back().count += static_cast<double>(count) * share;
    }
    return out;
}

}  // namespace fourfold

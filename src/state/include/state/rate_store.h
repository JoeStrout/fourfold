// RateStore: rate-limit counters, and proof-of-work puzzles already used
// (overview §5, Rate limits and spam; Arch §5 state), in private/state/.
//
// Counters are kept in SQLite, not only in memory, so they hold across
// restarts and across the separate processes of CGI mode. Each limit is a
// sliding window, estimated from two fixed ones: the count in the current
// window, plus the previous window's count scaled by how much of it the
// sliding window still covers. That smooths the burst a fixed window allows
// at its boundary, at the cost of two rows per key.
//
// Every method blocks on SQLite, so it's for worker threads; the store
// serializes its own calls.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/result.h"
#include "platform/sqlite.h"

namespace fourfold {

// One limit to count a request against.
struct RateCheck {
    std::string key;       // what's counted together: the rule, and who ("default|ip:1.2.3.4")
    int64_t count = 0;     // at most this many...
    int64_t seconds = 60;  // ...in this long
};

struct RateResult {
    bool allowed = true;
    // When refused: the first check over its limit, and about how long until
    // a request would be allowed again (whole seconds, at least 1).
    size_t refusedIndex = 0;
    int64_t retryAfterSeconds = 0;
    // Each check's estimated count, including this request if it was allowed.
    std::vector<double> counts;
};

class RateStore {
public:
    // Opens (creating) the tables in the database at `path`
    // (StateDir::sqlitePath). The clock must outlive the store.
    static Result<std::unique_ptr<RateStore>> open(const std::string& path, const Clock& clock = systemClock());

    // Counts one request against every check, if all of them allow it; if
    // any is over its limit, counts nothing and says which. In one
    // transaction, so concurrent requests (and processes) count exactly.
    Result<RateResult> hit(const std::vector<RateCheck>& checks);

    // Records a proof-of-work puzzle as used, until `expiresAtMs`; false if
    // it was already used.
    Result<bool> useOnce(const std::string& id, int64_t expiresAtMs);

    // Removes counters and used puzzles that no longer matter.
    Result<int64_t> sweep();
    // Removes every counter (the dev tools' "reset limits").
    Status resetCounters();

    // Each counter's key and current estimate, by key (for the dev tools).
    struct Counter {
        std::string key;
        int64_t seconds = 0;
        double count = 0;
    };
    Result<std::vector<Counter>> counters();

private:
    RateStore(std::unique_ptr<Sqlite> db, const Clock& clock) : db_(std::move(db)), clock_(clock) {}

    std::mutex mutex_;
    std::unique_ptr<Sqlite> db_;
    const Clock& clock_;
};

}  // namespace fourfold

// AuthStore: the engine's users and sign-in tokens, in private/state/
// (overview §5, "Built into the engine"; Arch §5 state). App code never
// reaches it: it sees only `req.user`, and the `users` module's lookups and
// role changes, which the engine makes on its behalf.
//
// It holds identity only (id, email, name, roles); an app's own data about
// a user goes in data/, keyed by the user's id.
//
// Magic-link tokens are 32 random bytes, sent only in the email; the store
// keeps a hash. Each is good once, for a short time, and creating them is
// throttled per email address and per visitor address, so the sign-in form
// can't be used to flood someone's inbox.
//
// Every method blocks on SQLite, so it's for worker threads; the store
// serializes its own calls.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/result.h"
#include "platform/sqlite.h"

namespace fourfold {

struct AuthStoreConfig {
    // How long an emailed link works.
    std::chrono::milliseconds tokenLifetime = std::chrono::minutes(15);
    // Links sent per email address, and per visitor address group, within
    // `throttleWindow`; more are refused.
    int maxTokensPerEmail = 3;
    int maxTokensPerAddress = 10;
    std::chrono::milliseconds throttleWindow = std::chrono::hours(1);
};

struct AuthUser {
    std::string id;                    // random, stable, opaque
    std::string email;                 // lowercased
    std::string name;
    int64_t createdAt = 0;             // ms since the epoch
    std::vector<std::string> roles;    // sorted
};

// A redeemed sign-in token: who it was for, and where to go afterward.
struct LoginToken {
    std::string email;
    std::string next;   // a path on this site, "/" if none
};

class AuthStore {
public:
    // Opens (creating) the tables in the database at `path`
    // (StateDir::sqlitePath). The clock must outlive the store.
    static Result<std::unique_ptr<AuthStore>> open(const std::string& path, AuthStoreConfig config = {},
                                                   const Clock& clock = systemClock());

    // A changed config (overview §10: config reloads), for calls from now on.
    void setConfig(AuthStoreConfig config);

    // An email address as stored: trimmed and lowercased.
    static std::string normalizeEmail(std::string_view email);

    Result<std::optional<AuthUser>> get(const std::string& id);
    Result<std::optional<AuthUser>> findByEmail(std::string_view email);
    // A new user; Conflict if the email address has one.
    Result<AuthUser> create(std::string_view email, const std::string& name);
    // NotFound if there's no such user.
    Status grant(const std::string& id, const std::string& role);
    Status revoke(const std::string& id, const std::string& role);
    Status setName(const std::string& id, const std::string& name);
    // Every user, by email (for the dev tools and tests).
    Result<std::vector<AuthUser>> all();

    // A sign-in token for `email`, to send in a link, or nullopt if the
    // throttle refuses it. `next` is where to go after signing in.
    Result<std::optional<std::string>> createToken(std::string_view email, const std::string& addressGroup,
                                                   const std::string& next);
    // Uses up `token`: what it was for, or nullopt if it's unknown, used or
    // expired.
    Result<std::optional<LoginToken>> redeemToken(const std::string& token);

    // Removes expired tokens that no longer count toward the throttles.
    Result<int64_t> sweep();

private:
    AuthStore(std::unique_ptr<Sqlite> db, AuthStoreConfig config, const Clock& clock)
        : db_(std::move(db)), config_(config), clock_(clock) {}

    Result<std::optional<AuthUser>> loadWhere(const char* sql, const std::string& key);
    Status userExists(const std::string& id);

    std::mutex mutex_;
    std::unique_ptr<Sqlite> db_;
    AuthStoreConfig config_;   // guarded by mutex_
    const Clock& clock_;
};

}  // namespace fourfold

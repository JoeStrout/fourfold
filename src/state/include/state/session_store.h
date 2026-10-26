// SessionStore: session records in private/state/ (overview §2, Session
// storage; Arch §5 state).
//
// A session's ID is 32 random bytes, sent only in the visitor's cookie. The
// store keeps a hash of it, never the ID itself, so a leaked store (a backup,
// a disk image) gives no IDs that could take over a live session.
//
// Each record has two parts, both as canonical JSON bytes: the engine's
// (flash, and later the CSRF state of signed-in users) and the app's (the
// `session` map). The store doesn't look inside either.
//
// Every method blocks on SQLite, so it's for worker threads; the store
// serializes its own calls, and may be shared between them.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/clock.h"
#include "base/result.h"
#include "platform/sqlite.h"

namespace fourfold {

struct SessionConfig {
    // Timeouts (overview §2, Timeouts).
    std::chrono::milliseconds anonymousIdle = std::chrono::hours(24);
    std::chrono::milliseconds anonymousLimit = std::chrono::hours(24 * 7);
    std::chrono::milliseconds signedInIdle = std::chrono::hours(24 * 14);
    std::chrono::milliseconds signedInLimit = std::chrono::hours(24 * 30);
    // "Last used" is written at most this often, so a page view doesn't
    // cost a write.
    std::chrono::milliseconds touchInterval = std::chrono::minutes(5);
    // The app's part, serialized; over this, the request fails and nothing
    // is saved.
    size_t maxBytes = 64 * 1024;
    // Limits against bots: anonymous sessions per address group and in all.
    // Over either, the oldest anonymous sessions (of that group, or of all)
    // are removed. Signed-in sessions are never removed for this.
    size_t maxAnonymousPerAddress = 1000;
    size_t maxAnonymous = 100'000;
};

struct SessionRecord {
    std::string idHash;                  // hashBytes(ID): 32 raw bytes
    std::optional<std::string> userId;   // signed in, if set (M6)
    int64_t createdAt = 0;               // ms since the epoch
    int64_t lastUsedAt = 0;
    std::string engine;                  // canonical JSON (a map)
    std::string app;                     // canonical JSON (a map)
};

class SessionStore {
public:
    // Opens (creating) the database at `path` (StateDir::sqlitePath). The
    // clock must outlive the store.
    static Result<std::unique_ptr<SessionStore>> open(const std::string& path, SessionConfig config,
                                                      const Clock& clock = systemClock());

    const SessionConfig& config() const { return config_; }

    // A new random ID, for a cookie: base64url of 32 bytes.
    static std::string newId();
    // The hash a cookie's ID is stored under, or nullopt if `cookieValue`
    // isn't a well-formed ID.
    static std::optional<std::string> hashOfId(std::string_view cookieValue);

    // The live session with this hash, or nullopt if there's none or it has
    // expired (an expired one is removed).
    Result<std::optional<SessionRecord>> load(const std::string& idHash);

    // Stores a new anonymous session for `id` (from newId), first making
    // room under the caps. `addressGroup` is the visitor's address (IPv4) or
    // /64 (IPv6).
    Status create(const std::string& id, const std::string& addressGroup, const std::string& engine,
                  const std::string& app);

    // Replaces both parts, and marks the session used now. NotFound if it
    // has gone (expired, or signed out meanwhile).
    Status save(const std::string& idHash, const std::string& engine, const std::string& app);

    // Replaces the engine's part only if it's still `expected` (e.g. to clear
    // a flash message that a page without code showed, without taking the
    // session's lock). Returns whether it did.
    Result<bool> replaceEngine(const std::string& idHash, const std::string& expected, const std::string& engine);

    // Marks the session used now, if it's been `touchInterval` since the last
    // time. `lastUsedAt` is the record's, as loaded.
    Status touch(const std::string& idHash, int64_t lastUsedAt);

    Status remove(const std::string& idHash);
    // Every session of this user ("sign out everywhere"; M6).
    Status removeForUser(const std::string& userId);

    // Removes expired sessions; returns how many.
    Result<int64_t> sweepExpired();

    // For tests and the dev tools.
    Result<int64_t> count();

private:
    SessionStore(std::unique_ptr<Sqlite> db, SessionConfig config, const Clock& clock)
        : db_(std::move(db)), config_(std::move(config)), clock_(clock) {}

    bool expired(const SessionRecord& r, int64_t now) const;
    Status evictAnonymous(const std::string& addressGroup);

    std::mutex mutex_;
    std::unique_ptr<Sqlite> db_;
    SessionConfig config_;
    const Clock& clock_;
};

}  // namespace fourfold

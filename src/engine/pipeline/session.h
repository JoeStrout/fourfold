// The engine's side of a request's session (overview §2, Session storage):
// finding it from the cookie, locking and loading it for a run, and saving
// what the run did. Private to the pipeline.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/datum.h"
#include "http/message.h"
#include "platform/worker_pool.h"
#include "script/script_host.h"
#include "state/lock_service.h"
#include "state/session_store.h"

namespace fourfold {

// The value of cookie `name` in the request's Cookie headers, if any.
std::optional<std::string> cookieValue(const HttpRequest& request, const std::string& name);

// A cookie's name: "__Host-ff_<base>" over HTTPS, which the browser then
// keeps to this host and to HTTPS; "ff_<base>" otherwise (dev mode).
std::string cookieName(const std::string& base, bool https);

// A Set-Cookie value: Path=/, HttpOnly, SameSite=Lax, and Secure over HTTPS.
std::string setCookie(const std::string& base, const std::string& value, bool https, int64_t maxAgeSeconds);

// The visitor's address group, for the anonymous-session caps: an IPv4
// address as is, an IPv6 address's /64 (overview §5, Rate limits: IPv6
// addresses are handed out by the /64).
std::string addressGroup(const std::string& peerAddress);

// One request's session: SessionAccess for the run, then commit() after it.
// open() and commit() may be called on any thread. Made with make_shared:
// work in progress keeps it alive.
class EngineSession : public SessionAccess, public std::enable_shared_from_this<EngineSession> {
public:
    // `idHash`: the hash of the ID in the request's cookie, if it had a
    // well-formed one. The store, locks and pool must outlive this.
    EngineSession(SessionStore& store, LockService& locks, WorkerPool& workers, std::optional<std::string> idHash,
                  std::string addressGroup, bool https);
    ~EngineSession() override;

    void open(std::function<void(Result<SessionInput>)> ready) override;

    struct Committed {
        std::optional<std::string> setCookie;   // a new session's cookie
        std::optional<Error> error;             // saving failed: the response becomes a 500
    };
    // Blocks (SQLite): call on a worker. Saves what the run did, if it ran to
    // the end (`result` set), then releases the lock. Call exactly once.
    Committed commit(const std::optional<SessionResult>& result);

private:
    SessionStore& store_;
    LockService& locks_;
    WorkerPool& workers_;
    std::optional<std::string> idHash_;
    std::string addressGroup_;
    bool https_;

    std::mutex mutex_;
    std::optional<LockService::Ticket> ticket_;   // set while holding (or waiting for) the lock
    std::optional<SessionRecord> record_;         // the session as loaded; none for a new visitor
};

}  // namespace fourfold

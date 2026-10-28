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
#include "pipeline/cookies.h"
#include "platform/worker_pool.h"
#include "script/script_host.h"
#include "state/lock_service.h"
#include "state/session_store.h"

namespace fourfold {

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

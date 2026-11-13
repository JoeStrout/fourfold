// Observer: the one-way event interface from the core to the dev tools
// (Arch §5 devtools, §6 Observer). The core reports what happens; it never
// reads anything back. In production the observer is absent or a no-op.
//
// Events may come from any thread (the lifecycle thread, the script thread,
// the I/O loop); an implementation must be thread-safe.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "partition/paths.h"
#include "script/script_host.h"

namespace fourfold {

// One compile of the app, finished.
struct CompileReport {
    bool ok = false;
    // The generation serving after this compile: the new one if it
    // succeeded, else the one still serving (0 if there's none yet).
    uint64_t generation = 0;
    std::vector<Diagnostic> diagnostics;   // errors and warnings, in the order found
    // Files in code/ added, changed or removed since the last compile
    // (empty for the first).
    std::vector<RelPath> changed;
    // Only existing stylesheets changed, so a page can swap them in place
    // instead of reloading.
    bool cssOnly = false;
    std::chrono::milliseconds duration{0};
    // Migrations (overview §3): those applied before the new version was
    // swapped in (dev mode, or `fourfold migrate`), and the backup taken
    // first; or, in production, how many are waiting for `fourfold migrate`
    // (the new version isn't serving until then).
    std::vector<int> migrated;
    std::string backup;
    size_t pendingMigrations = 0;
    bool seeded = false;   // code/seed.ms ran (dev mode, on a fresh database)
};

class Observer {
public:
    virtual ~Observer() = default;

    virtual void compileStarted() {}
    virtual void compileFinished(const CompileReport&) {}
    // A script failed while handling a request.
    virtual void scriptError(const ScriptError&) {}
    // Rendering a request's templates found fields with no value, or values
    // with no field (overview §4, "Missing and extra fields"). `generation`
    // is the app generation that rendered them.
    virtual void renderWarnings(uint64_t /*generation*/, const std::vector<RenderWarning>&) {}
    // A request ("POST /guestbook/sign") was counted against rate limits:
    // each limit, described, with its count after the request (overview §5,
    // Dev mode: the request inspector).
    virtual void limitsCounted(const std::string& /*request*/,
                               const std::vector<std::pair<std::string, double>>& /*counts*/) {}
    // App code sent (or tried to send) a request to another server
    // (overview §6, Visibility). Never holds a secret's value.
    virtual void outboundCall(const OutboundCall&) {}
    // A browser reported a Content Security Policy violation (overview §8):
    // what was blocked, and where.
    virtual void cspViolation(const std::string& /*description*/) {}
    // A request or job (`source`: "POST /signup", "job digest") enqueued
    // jobs, which were committed (`committed`), or dropped because it
    // failed (overview §2, Enqueuing).
    virtual void jobsEnqueued(const std::string& /*source*/, const std::vector<std::string>& /*names*/,
                              bool /*committed*/) {}
};

// Passes each event on to several observers, in order.
class ObserverList : public Observer {
public:
    // The observers must outlive the list.
    void add(Observer* observer) { observers_.push_back(observer); }

    void compileStarted() override;
    void compileFinished(const CompileReport& report) override;
    void scriptError(const ScriptError& error) override;
    void renderWarnings(uint64_t generation, const std::vector<RenderWarning>& warnings) override;
    void limitsCounted(const std::string& request, const std::vector<std::pair<std::string, double>>& counts) override;
    void outboundCall(const OutboundCall& call) override;
    void cspViolation(const std::string& description) override;
    void jobsEnqueued(const std::string& source, const std::vector<std::string>& names, bool committed) override;

private:
    std::vector<Observer*> observers_;   // fixed once events start
};

}  // namespace fourfold

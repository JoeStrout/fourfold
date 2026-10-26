// Observer: the one-way event interface from the core to the dev tools
// (Arch §5 devtools, §6 Observer). The core reports what happens; it never
// reads anything back. In production the observer is absent or a no-op.
//
// Events may come from any thread (the lifecycle thread, the script thread,
// the I/O loop); an implementation must be thread-safe.
#pragma once

#include <chrono>
#include <cstdint>
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

private:
    std::vector<Observer*> observers_;   // fixed once events start
};

}  // namespace fourfold

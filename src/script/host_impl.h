// ScriptHost's internals, shared by the files of the script component.
// Everything here belongs to the script thread unless marked otherwise.
#pragma once

#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>

#include "bundle.h"
#include "ms2.h"
#include "platform/event_loop.h"
#include "platform/worker_pool.h"
#include "script/script_host.h"

namespace fourfold {

// One VM run: a request's handler, or (at compile time) a page's top level.
struct Run {
    enum class Kind {
        Request,     // a handler, for one request; may park
        Discovery,   // a page's top level at compile time; may not park
    };
    enum class Phase { TopLevel, Handler };

    uint64_t id = 0;
    Kind kind = Kind::Request;
    Phase phase = Phase::TopLevel;
    ScriptHost::Impl* host = nullptr;
    std::string fileName;                   // "code/pages/hello.ms", for errors and print
    const std::map<std::string, MiniScript::FuncDef>* libs = nullptr;   // what `import` can find

    // Request runs only.
    std::shared_ptr<const CompiledApp> app;   // pins the bundle
    const ScriptBundle* bundle = nullptr;
    const PageProgram* page = nullptr;
    const HandlerStub* stub = nullptr;
    HandlerRef handler;
    RequestFacts request;
    ScriptHost::Done done;
    EventLoop::TimerId deadline = 0;

    MiniScript::Interpreter interp;
    uint64_t instructions = 0;   // counted in whole slices (see ScriptBudget)
    bool parked = false;         // waiting on a timer or a worker; not runnable

    // The run's state, for intrinsics: they reach it through hostData.
    static Run* of(MiniScript::Context& ctx);
};

struct ScriptHost::Impl {
    ScriptHostOptions options;
    std::unique_ptr<EventLoop> loop;       // the script thread's loop (post() is thread-safe)
    std::unique_ptr<WorkerPool> workers;   // submit() is thread-safe
    std::thread thread;

    // Script thread only.
    uint64_t nextRunId = 1;
    std::unordered_map<uint64_t, std::unique_ptr<Run>> runs;
    std::deque<uint64_t> runnable;
    bool pumpScheduled = false;

    // Starting and finishing request runs (scheduler.cpp).
    void startRun(std::shared_ptr<const CompiledApp> app, HandlerRef handler, RequestFacts request,
                  ScriptHost::Done done);
    void schedulePump();
    void pump();
    // Runs one slice of `run`; returns false once the run is finished.
    bool slice(Run& run);
    void finish(Run& run, HandlerOutcome outcome);

    // For async intrinsics: park the calling VM, and make it runnable again
    // later. resume() is safe to call after the run has finished or been
    // dropped; it then does nothing.
    void park(Run& run);
    void resume(uint64_t runId);

    // Compiling (compile.cpp).
    CompiledScripts compile(const std::vector<CodeFile>& pages, const std::vector<CodeFile>& libs);
};

// One-time MiniScript setup, and Fourfold's intrinsics (runtime.cpp). Call on
// the script thread before any other MiniScript use.
void initMiniScriptRuntime(const ScriptHostOptions& options);

// Sets the interpreter up for a run: output, host data, and the globals the
// engine provides before a page's top level runs (`get` and `post`).
void prepareInterpreter(Run& run, const MiniScript::List<MiniScript::FuncDef>& program);

// A located error from a MiniScript error value.
ScriptError scriptErrorFrom(MiniScript::Value error, const std::string& fallbackFile);

// The outcome of a finished handler phase (bridge.cpp), read from the run's
// `_ff_result` global.
HandlerOutcome outcomeFromResult(Run& run);

// The frozen `req` map (bridge.cpp).
MiniScript::Value makeRequestValue(const RequestFacts& facts);

}  // namespace fourfold

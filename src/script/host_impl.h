// ScriptHost's internals, shared by the files of the script component.
// Everything here belongs to the script thread unless marked otherwise.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>

#include "bundle.h"
#include "ms2.h"
#include "platform/event_loop.h"
#include "platform/worker_pool.h"
#include "script/script_host.h"
#include "state/lock_service.h"

namespace fourfold {

struct RawBudget;

// What work done off the script thread for an intrinsic brings back: plain
// data, turned into a MiniScript value when the intrinsic resumes.
struct AsyncResult {
    std::optional<Error> error;
    Datum value;
    std::string bytes;   // file.loadRaw: the file's bytes
    bool found = true;   // false: there was nothing to read
};

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
    RunContext context;                       // the request, render options, and services
    std::vector<RenderWarning> warnings;      // from frag(), so far
    ScriptHost::Done done;
    EventLoop::TimerId deadline = 0;

    // An async intrinsic's result, set just before the run resumes.
    std::optional<AsyncResult> async;
    // Data locks this run holds (data.update), by data name; released when
    // the run ends, however it ends.
    std::map<std::string, LockService::Ticket> dataLocks;
    // The names of the data.update calls in progress, innermost last.
    std::vector<RelPath> updates;
    // What this run's RawData buffers hold (made on first use).
    std::shared_ptr<RawBudget> rawBudget;

    // Discovery runs only: called as the run finishes, before its namespace
    // is released, with an Error outcome if the top level failed.
    std::function<void(Run&, const HandlerOutcome&)> discovered;

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
    void startRun(std::shared_ptr<const CompiledApp> app, HandlerRef handler, RunContext context,
                  ScriptHost::Done done);
    // Schedules a prepared run (a request or a discovery run), with its
    // wall-clock deadline.
    void schedule(std::unique_ptr<Run> run);
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
    // Thread-safe: hands `result` to a parked run and resumes it (on the
    // script thread). Does nothing if the run has ended.
    void complete(uint64_t runId, AsyncResult result);
    // The live run with this id, or null. Script thread only.
    Run* find(uint64_t runId);

    // Compiling (compile.cpp). One file at a time, each in its own posted
    // task, with each page's top level run as a scheduled discovery run, so
    // requests keep being served during a long compile. `done` is called on
    // the script thread. The files must stay alive until then.
    void compile(const std::vector<CodeFile>& pages, const std::vector<CodeFile>& libs,
                 std::function<void(CompiledScripts)> done);
};

// One-time MiniScript setup, and Fourfold's intrinsics (runtime.cpp). Call on
// the script thread before any other MiniScript use.
void initMiniScriptRuntime(const ScriptHostOptions& options);

// Sets the interpreter up for a run: output, host data, and the globals the
// engine provides before a page's top level runs (`get` and `post`).
void prepareInterpreter(Run& run, const MiniScript::List<MiniScript::FuncDef>& program);

// "return __ffCallback(__ffCallbackArg)", compiled once (runtime.cpp): how an
// intrinsic calls a script function without breaking the VM's time slicing.
// Set the two globals, then push it with ManuallyPushCall.
MiniScript::FuncDef callbackStub();

// A located error from a MiniScript error value.
ScriptError scriptErrorFrom(MiniScript::Value error, const std::string& fallbackFile);

// The outcome of a finished handler phase (bridge.cpp), read from the run's
// `_ff_result` global; for a session run, with what it did to the session
// (or an Error, if the session can't be saved).
HandlerOutcome outcomeFromResult(Run& run);

// The frozen `req` map (bridge.cpp).
MiniScript::Value makeRequestValue(const RequestFacts& facts);

// Adds `more` to a run's warnings, leaving out repeats.
void addWarnings(Run& run, const std::vector<RenderWarning>& more);

}  // namespace fourfold

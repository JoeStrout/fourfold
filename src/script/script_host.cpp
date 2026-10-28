// ScriptHost: the script thread and its scheduler (Arch §3).
//
// Runnable VMs take turns, one slice each, round-robin. A slice is
// `sliceInstructions` instructions, or less if the VM finishes, yields, or
// parks. The scheduler runs slices in batches of a couple of milliseconds,
// then lets the loop handle timers and posted work (new runs, finished worker
// tasks) before the next batch.

#include <atomic>
#include <future>

#include "bridge.h"
#include "host_impl.h"

namespace fourfold {

namespace {

constexpr auto kPumpBatch = std::chrono::milliseconds(2);

const char* const kTopLevelOverBudget =
    "the top level didn't finish within its budget; it should only define functions";
const char* const kDataOverBudget =
    "didn't finish within its budget; a declaration or config file should only build maps and lists";

// The over-budget message for a run of this kind.
std::string overBudget(const Run& run, const std::string& requestMessage) {
    if (run.kind == Run::Kind::Discovery) return kTopLevelOverBudget;
    if (run.kind == Run::Kind::Data) return kDataOverBudget;
    return requestMessage;
}

std::atomic<bool> g_hostExists{false};

HandlerOutcome errorOutcome(std::string message, std::string file) {
    HandlerOutcome o;
    o.kind = HandlerOutcome::Kind::Error;
    o.error.message = std::move(message);
    o.error.file = std::move(file);
    return o;
}

std::string describeMs(std::chrono::milliseconds ms) {
    if (ms.count() % 1000 == 0) return std::to_string(ms.count() / 1000) + " s";
    return std::to_string(ms.count()) + " ms";
}

}  // namespace

// --- On the script thread ---

void ScriptHost::Impl::startRun(std::shared_ptr<const CompiledApp> app, HandlerRef handler, RunContext context,
                                ScriptHost::Done done) {
    const ScriptBundle* bundle = app ? app->scripts.get() : nullptr;
    const PageProgram* page = bundle ? bundle->page(handler.source) : nullptr;
    const HandlerStub* stub = bundle ? bundle->stub(handler) : nullptr;
    if (!page || !stub) {
        done(errorOutcome("no compiled handler " + handler.label() + " in code/" + handler.source.str(),
                          "code/" + handler.source.str()));
        return;
    }

    auto run = std::make_unique<Run>();
    run->kind = Run::Kind::Request;
    run->host = this;
    run->fileName = page->fileName;
    run->libs = &bundle->libs;
    run->app = std::move(app);
    run->bundle = bundle;
    run->page = page;
    run->stub = stub;
    run->handler = std::move(handler);
    run->context = std::move(context);
    run->done = std::move(done);
    prepareInterpreter(*run, page->functions);
    std::shared_ptr<SessionAccess> session = run->context.session;
    if (!session) {
        schedule(std::move(run));
        return;
    }

    // Load the session (after its lock) before any of the page's code runs,
    // parked meanwhile; the wall-clock budget covers the wait.
    run->parked = true;
    uint64_t id = nextRunId;   // schedule() gives it this id
    schedule(std::move(run));
    session->open([this, id](Result<SessionInput> input) {
        auto shared = std::make_shared<Result<SessionInput>>(std::move(input));
        loop->post([this, id, shared] {
            Run* r = find(id);
            if (!r) return;
            if (!*shared) {
                finish(*r, errorOutcome("the session couldn't be loaded: " + shared->error().message, r->fileName));
                return;
            }
            r->interp.SetGlobalValue("session", toValue(shared->value().app));
            resume(id);
        });
    });
}

void ScriptHost::Impl::schedule(std::unique_ptr<Run> run) {
    run->id = nextRunId++;
    uint64_t id = run->id;
    run->deadline = loop->addTimer(options.budget.wallClock, [this, id] {
        auto it = runs.find(id);
        if (it == runs.end()) return;
        Run& r = *it->second;
        r.deadline = 0;
        finish(r, errorOutcome(overBudget(r, "the request took longer than its time budget (" +
                                                 describeMs(options.budget.wallClock) + ")"),
                               r.fileName));
    });
    bool parked = run->parked;   // a run waiting for its session starts parked
    runs.emplace(id, std::move(run));
    if (!parked) {
        runnable.push_back(id);
        schedulePump();
    }
}

void ScriptHost::Impl::schedulePump() {
    if (pumpScheduled) return;
    pumpScheduled = true;
    loop->post([this] { pump(); });
}

void ScriptHost::Impl::pump() {
    pumpScheduled = false;
    auto start = EventLoop::Clock::now();
    while (!runnable.empty()) {
        uint64_t id = runnable.front();
        runnable.pop_front();
        auto it = runs.find(id);
        if (it == runs.end() || it->second->parked) continue;
        if (slice(*it->second)) runnable.push_back(id);
        if (EventLoop::Clock::now() - start >= kPumpBatch) break;
    }
    // A GC tick (MiniScript counts ticks to decide when to collect).
    MiniScript::GCManager::MaybeCollect(false);
    if (!runnable.empty()) schedulePump();
}

bool ScriptHost::Impl::slice(Run& run) {
    MiniScript::VM vm = run.interp.vm();
    vm.set_yielding(false);
    vm.Run(options.sliceInstructions);
    run.instructions += options.sliceInstructions;

    if (!vm.Error().IsNull()) {
        HandlerOutcome o;
        o.kind = HandlerOutcome::Kind::Error;
        o.error = scriptErrorFrom(vm.Error(), run.fileName);
        finish(run, std::move(o));
        return false;
    }
    if (!vm.IsRunning()) {
        if (run.kind == Run::Kind::Discovery || run.kind == Run::Kind::Data) {
            finish(run, HandlerOutcome{});   // the top level ran; `discovered` reads its globals
            return false;
        }
        if (run.phase == Run::Phase::TopLevel) {
            // The top level has defined the page's functions; now call the
            // handler, in the same namespace, through its stub.
            run.phase = Run::Phase::Handler;
            run.interp.SetGlobalValue("_ff_req", makeRequestValue(run.context.request));
            vm.Reset(run.stub->functions, run.interp.GetGlobals());
            return true;
        }
        finish(run, outcomeFromResult(run));
        return false;
    }
    uint64_t limit = run.instructionLimit ? run.instructionLimit : options.budget.instructions;
    if (run.instructions >= limit) {
        finish(run, errorOutcome(overBudget(run, "the request used more than its budget of " +
                                                     std::to_string(options.budget.instructions) +
                                                     " instructions (an endless loop?)"),
                                 run.fileName));
        return false;
    }
    return !run.parked;
}

void ScriptHost::Impl::finish(Run& run, HandlerOutcome outcome) {
    if (run.deadline) loop->cancelTimer(run.deadline);
    // Locks a data.update was holding when the run ended (an error, or the
    // budget ran out mid-update).
    for (const auto& [name, ticket] : run.dataLocks) {
        if (run.context.data) run.context.data->locks().release(ticket);
    }
    run.dataLocks.clear();
    ScriptHost::Done done = std::move(run.done);
    if (run.discovered) run.discovered(run, outcome);
    run.interp.Reset();   // releases the namespace; the VM goes with the interpreter
    run.interp = nullptr;
    runs.erase(run.id);   // `run` is gone after this
    if (done) done(std::move(outcome));
}

void ScriptHost::Impl::park(Run& run) {
    run.parked = true;
}

Run* ScriptHost::Impl::find(uint64_t runId) {
    auto it = runs.find(runId);
    return it == runs.end() ? nullptr : it->second.get();
}

void ScriptHost::Impl::complete(uint64_t runId, AsyncResult result) {
    auto shared = std::make_shared<AsyncResult>(std::move(result));
    loop->post([this, runId, shared] {
        Run* r = find(runId);
        if (!r || !r->parked) return;
        r->async = std::move(*shared);
        resume(runId);
    });
}

void ScriptHost::Impl::resume(uint64_t runId) {
    auto it = runs.find(runId);
    if (it == runs.end() || !it->second->parked) return;
    it->second->parked = false;
    runnable.push_back(runId);
    schedulePump();
}

// --- ScriptHost ---

Result<std::unique_ptr<ScriptHost>> ScriptHost::create(ScriptHostOptions options) {
    if (g_hostExists.exchange(true)) {
        return makeError(ErrorCode::Conflict, "only one ScriptHost may exist at a time");
    }
    auto loop = EventLoop::create();
    if (!loop) {
        g_hostExists = false;
        return loop.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);
    if (impl->options.sliceInstructions == 0) impl->options.sliceInstructions = 1;
    impl->loop = std::move(loop).value();
    impl->workers = std::make_unique<WorkerPool>(impl->options.workerThreads);

    Impl* p = impl.get();
    std::promise<void> started;
    impl->thread = std::thread([p, &started] {
        setBundleDisposalLoop(p->loop.get());
        initMiniScriptRuntime(p->options);
        started.set_value();
        p->loop->run();
        // Stopped: abandon unfinished runs, and run what's been posted (e.g.
        // bundles to dispose of) once more before letting go of the thread.
        for (auto& [id, run] : p->runs) {
            run->interp.Reset();
            run->interp = nullptr;
        }
        p->runs.clear();
        p->runnable.clear();
        p->loop->runOnce(std::chrono::milliseconds(0));
        setBundleDisposalLoop(nullptr);
    });
    started.get_future().wait();
    return std::unique_ptr<ScriptHost>(new ScriptHost(std::move(impl)));
}

ScriptHost::ScriptHost(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ScriptHost::~ScriptHost() {
    impl_->loop->stop();
    impl_->thread.join();
    impl_->workers.reset();   // workers may still post to the loop, which outlives them
    impl_.reset();
    g_hostExists = false;
}

CompiledScripts ScriptHost::compileScripts(const std::vector<CodeFile>& pages, const std::vector<CodeFile>& libs) {
    // If the host is destroyed mid-compile, the promise is broken, not kept.
    auto finished = std::make_shared<std::promise<CompiledScripts>>();
    std::future<CompiledScripts> result = finished->get_future();
    Impl* impl = impl_.get();
    impl->loop->post([impl, &pages, &libs, finished] {
        impl->compile(pages, libs, [finished](CompiledScripts out) { finished->set_value(std::move(out)); });
    });
    try {
        return result.get();
    } catch (const std::future_error&) {
        CompiledScripts out;
        out.diagnostics.push_back({Diagnostic::Severity::Error, "code", 0, "the script host stopped while compiling"});
        return out;
    }
}

void ScriptHost::run(std::shared_ptr<const CompiledApp> app, HandlerRef handler, const GateApproval&,
                     RunContext context, Done done) {
    // std::function needs a copyable callable, so the arguments ride in a
    // shared struct.
    struct Args {
        std::shared_ptr<const CompiledApp> app;
        HandlerRef handler;
        RunContext context;
        Done done;
    };
    auto args = std::make_shared<Args>(Args{std::move(app), std::move(handler), std::move(context), std::move(done)});
    Impl* impl = impl_.get();
    impl->loop->post([impl, args] {
        impl->startRun(std::move(args->app), std::move(args->handler), std::move(args->context),
                       std::move(args->done));
    });
}

void ScriptHost::setBudget(ScriptBudget budget) {
    Impl* impl = impl_.get();
    impl->loop->post([impl, budget] {
        impl->options.budget = budget;
        MiniScript::Value::MAX_COLLECTION_SIZE = budget.maxCollectionSize;
    });
}

WorkerPool& ScriptHost::workers() { return *impl_->workers; }

ScriptHost::Stats ScriptHost::stats(bool collectFirst) {
    Stats s;
    std::promise<void> finished;
    impl_->loop->post([&] {
        if (collectFirst) MiniScript::GCManager::FullCollectGarbage();
        s.activeRuns = impl_->runs.size();
        for (const auto& [id, run] : impl_->runs) s.parkedRuns += run->parked ? 1 : 0;
        s.liveMaps = MiniScript::GCManager::Maps.LiveCount();
        s.liveLists = MiniScript::GCManager::Lists.LiveCount();
        s.liveFunctions = MiniScript::GCManager::Functions.LiveCount();
        s.liveBundles = ScriptBundle::liveCount();
        finished.set_value();
    });
    finished.get_future().wait();
    return s;
}

}  // namespace fourfold

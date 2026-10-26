// ScriptHost: everything that runs MiniScript (Arch §3, §5 `script`).
//
// MiniScript 2's garbage collector is process-wide and unsynchronized, so
// every MiniScript value lives on one thread, the script thread, which this
// class owns. Request VMs are interleaved on it in time slices; a VM that
// waits (on a timer, or on work done by the worker pool) is parked, and the
// thread runs other VMs meanwhile.
//
// Nothing in this header mentions MiniScript types. What crosses into and out
// of the script thread is plain C++ data: CodeFile in, CompiledScripts and
// HandlerOutcome out, RequestFacts in.
//
// Only one ScriptHost may exist at a time in a process (MS2's state is
// global). Its methods may be called from any thread.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "appdata/data_files.h"
#include "base/datum.h"
#include "compiledapp/compiled_app.h"
#include "partition/code_tree.h"
#include "partition/data_dir.h"
#include "platform/worker_pool.h"
#include "template/template.h"

namespace fourfold {

// Per-run limits (overview §2, "Resource budgets"; Arch §1).
struct ScriptBudget {
    // Instructions per run, counted in whole slices: MS2 doesn't report how
    // many instructions a slice actually ran, so a run may stop up to one
    // slice early (never late).
    uint64_t instructions = 10'000'000;
    // Wall-clock time per run, including time parked.
    std::chrono::milliseconds wallClock{10'000};
    // The longest string or list that repetition or concatenation may build
    // (MS2's MAX_COLLECTION_SIZE). Process-wide in MS2.
    int maxCollectionSize = 16 * 1024 * 1024;
    // RawData buffers live outside MS2's heap: the largest one (the same as
    // the largest file in data/), and all of one run's together.
    size_t maxRawDataBytes = 16 * 1024 * 1024;
    size_t maxRawDataTotal = 64 * 1024 * 1024;
};

struct ScriptHostOptions {
    ScriptBudget budget;
    // Instructions per time slice: how long one VM runs before the next gets
    // a turn.
    uint32_t sliceInstructions = 10'000;
    // Threads for work that would block the script thread.
    size_t workerThreads = 4;
    // Registers the _test* intrinsics (for tests and sample apps).
    bool testIntrinsics = false;
    // Where `print` goes: the file that printed, and the text. Default: dropped.
    std::function<void(const std::string& file, const std::string& text)> printOutput;
};

// What a handler sees of a request (`req`, overview §4). Plain data, built by
// the pipeline; the script thread turns it into a frozen map.
struct RequestFacts {
    using Pairs = std::vector<std::pair<std::string, std::string>>;

    std::string method;    // "GET"
    std::string path;      // canonical, "/guestbook"
    Pairs query;           // decoded; for a repeated name, the first wins
    Pairs headers;         // names lowercased; repeated names joined with ", "
    Pairs form;            // decoded url-encoded POST body; raw strings
};

// The visitor's session as a run starts (overview §2, Session storage).
struct SessionInput {
    Datum app = Datum::map({});   // the `session` map
};

// What a run did to the session, for the engine to save after it.
struct SessionResult {
    // The `session` map at the end, as canonical JSON, checked to be inert
    // and within the size cap.
    // A page with a {{flash}} field was rendered, so `session.flash` (if
    // any) has been shown, and is left out of `app`.
    std::string app;
};

// The engine's side of a run's session: loading it (after taking its lock).
// The script host calls open() once, on the script thread, before the
// page's code runs; the run waits, parked, under its wall-clock budget,
// until `ready` is called (from any thread). Saving is the engine's, after
// the run.
class SessionAccess {
public:
    virtual ~SessionAccess() = default;
    virtual void open(std::function<void(Result<SessionInput>)> ready) = 0;
};

// Proof that the policy gate allowed a request (Arch §7, "Access is checked
// before any handler"): ScriptHost::run requires one, and only PolicyGate can
// make one. (Tests make theirs through TestGate, in tests/support.)
class GateApproval {
private:
    GateApproval() = default;
    friend class PolicyGate;
    friend struct TestGate;
};

// Everything one run may use (Arch §5, "The RunContext"): the request, and
// the services the engine gives this kind of run. A module whose service is
// null isn't available to the run (its functions return an error).
struct RunContext {
    RequestFacts request{};
    // The response's CSP nonce and the Markdown stylesheet, for the page and
    // every frag() the run renders.
    RenderOptions render{};
    // Set for a route that uses `session` (its code names it, or its page
    // has a {{flash}} field). Must stay alive
    // until the run is done.
    std::shared_ptr<SessionAccess> session{};
    // The largest `session` map, serialized; over it, the run fails.
    size_t sessionMaxBytes = 64 * 1024;
    // The app's data/ (the `data` module). Must outlive the run.
    DataFiles* data = nullptr;
    // The app's dropbox/ (the `dropbox` module). Must outlive the run.
    const Dropbox* dropbox = nullptr;
};

// A compile or runtime error in a script, located for the developer.
struct ScriptError {
    std::string message;    // "Undefined Identifier: 'x' is unknown in this context"
    std::string file;       // "code/pages/hello.ms"; empty if unknown
    int line = 0;           // 0 if unknown
};

// What a handler run produced (overview §4, Return values), converted to
// plain data before it leaves the script thread.
struct HandlerOutcome {
    enum class Kind {
        Render,      // render returned a map: `body` is its page, filled in
        Fragment,    // frag(...), markdown(...) or unsafeHTML(...): `body` is the HTML
        Text,        // a string: text/plain
        Redirect,    // redirect(url): 303
        Json,        // json(value)
        Status,      // status(code, body)
        NoContent,   // null: 204
        Error,       // an error value, a runtime error, or a budget overrun: 500
    };

    Kind kind = Kind::NoContent;
    std::string body;          // Render, Fragment, Text, Json, Status
    std::string contentType;   // Status, when it has a body
    // Fragment, or Status with an HTML body: it contains a <script>, which
    // can't be sent on its own (it wouldn't have the page's CSP nonce).
    bool hasScript = false;
    // Missing and unused fields, from rendering the page or any fragment.
    std::vector<RenderWarning> warnings;
    std::string location;      // Redirect
    int status = 0;            // Status
    ScriptError error;         // Error
    // If the run had session access and its handler finished without an
    // error: what it did to the session.
    std::optional<SessionResult> session;
};

// The handlers found in one page's .ms file, by running its top level once at
// compile time (overview §4, Handler model).
struct PageHandlers {
    bool render = false;
    std::vector<std::string> get;    // the keys of `get`, sorted
    std::vector<std::string> post;   // the keys of `post`, sorted
    // The file, or a code/lib/ file it imports, mentions `session`: its
    // runs get the session (loaded and locked). Requests to
    // pages that don't never wait for a session's lock.
    bool usesSession = false;
};

struct CompiledScripts {
    // Null if any file failed to compile.
    std::shared_ptr<const ScriptBundle> bundle;
    std::map<RelPath, PageHandlers> pages;   // by the page file's path ("pages/hello.ms")
    std::vector<Diagnostic> diagnostics;
};

// The compiler's view of the script side: .ms files in, a bundle out.
class ScriptCompiler {
public:
    virtual ~ScriptCompiler() = default;

    // `pages` are .ms files in code/pages/; `libs` are .ms files directly in
    // code/lib/, importable by name ("util" for lib/util.ms).
    virtual CompiledScripts compileScripts(const std::vector<CodeFile>& pages,
                                           const std::vector<CodeFile>& libs) = 0;
};

class ScriptHost : public ScriptCompiler {
public:
    using Done = std::function<void(HandlerOutcome)>;

    // Starts the script thread and the worker pool. Fails (Conflict) if
    // another ScriptHost exists.
    static Result<std::unique_ptr<ScriptHost>> create(ScriptHostOptions options = {});
    // Stops the script thread. Runs still in progress are abandoned, and their
    // Done callbacks never called.
    ~ScriptHost() override;

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Compiles on the script thread, and waits for the result.
    CompiledScripts compileScripts(const std::vector<CodeFile>& pages,
                                   const std::vector<CodeFile>& libs) override;

    // Runs `handler` from `app` for one request, in a fresh VM, with the
    // request and services in `context`. `done` is called exactly once, on
    // the script thread, unless the host is destroyed first. `app` stays
    // pinned (with its bytecode) until then.
    void run(std::shared_ptr<const CompiledApp> app, HandlerRef handler, const GateApproval& approval,
             RunContext context, Done done);

    // The pool that does work that would block the script thread. Engines
    // may share it (e.g. to save sessions).
    WorkerPool& workers();

    // For tests and measurements; each call runs on the script thread and waits.
    struct Stats {
        size_t activeRuns = 0;     // started, not yet done
        size_t parkedRuns = 0;     // waiting on a timer or the worker pool
        int liveMaps = 0;          // MS2 GC counts, after a full collection
        int liveLists = 0;
        int liveFunctions = 0;
        int liveBundles = 0;       // compiled .ms files of app generations not yet released
    };
    Stats stats(bool collectFirst = true);

    struct Impl;   // private

private:
    explicit ScriptHost(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace fourfold

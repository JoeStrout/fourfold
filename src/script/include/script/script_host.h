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
#include <string>
#include <utility>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "partition/code_tree.h"
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
};

// The handlers found in one page's .ms file, by running its top level once at
// compile time (overview §4, Handler model).
struct PageHandlers {
    bool render = false;
    std::vector<std::string> get;    // the keys of `get`, sorted
    std::vector<std::string> post;   // the keys of `post`, sorted
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

    // Runs `handler` from `app` for one request, in a fresh VM. `done` is
    // called exactly once, on the script thread, unless the host is destroyed
    // first. `app` stays pinned (with its bytecode) until then. Templates the
    // run renders (the page, and every frag()) use `render`: the response's
    // CSP nonce, and the Markdown stylesheet.
    void run(std::shared_ptr<const CompiledApp> app, HandlerRef handler, RequestFacts request, Done done,
             RenderOptions render = {});

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

// One-time MiniScript setup, and the intrinsics Fourfold adds or replaces.
//
// MS2's intrinsic registry is process-wide, and an intrinsic's code is copied
// into its function the first time a VM is built, so everything here happens
// once, on the script thread, before the first VM (see the MS2 notes in
// docs/design/m2-results.md).

#include <cctype>
#include <cmath>

#include "base/version.h"
#include "bridge.h"
#include "host_impl.h"
#include "modules/modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::ErrorTypes;
using MiniScript::FuncDef;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::String;
using MiniScript::Value;

Run* Run::of(Context& ctx) {
    MiniScript::Interpreter interp = ctx.GetInterpreter();
    if (IsNull(interp)) return nullptr;
    return static_cast<Run*>(interp.hostData());
}

namespace {

// "return __ffCallback(__ffCallbackArg)", compiled once: how an intrinsic
// calls a script function with an argument without breaking the VM's time
// slicing (see _testWithValue).
FuncDef g_callbackStub;

IntrinsicResult errorResult(const std::string& message) {
    return IntrinsicResult(ErrorTypes::RuntimeError(String(message.data(), message.size())));
}

// Replaces a built-in intrinsic with one that returns an error value.
void disable(const char* name, const char* why) {
    std::string message = std::string(name) + " isn't available in Fourfold: " + why;
    Intrinsic f = Intrinsic::GetByName(name);
    if (IsNull(f)) return;
    f.set_Code([message](Context, IntrinsicResult) { return errorResult(message); });
}

// gc returns a map of functions (gc.collect, gc.stats); replace it with one
// whose functions explain, rather than an error that gc.collect would hide.
void disableGc() {
    std::string message = "gc isn't available in Fourfold: collection is managed by the engine";
    Value map = Value::make_map(2);
    for (const char* name : {"collect", "stats"}) {
        Intrinsic f = Intrinsic::Create("");
        f.AddParam("full", Value::zero);
        f.set_Code([message](Context, IntrinsicResult) { return errorResult(message); });
        map.MapSet(name, f.GetFunc());
    }
    map.Freeze();
    MiniScript::GCManager::AddRoot(map);   // for the life of the process
    Intrinsic::GetByName("gc").set_Code([map](Context, IntrinsicResult) { return IntrinsicResult(map); });
}

// A request run that may park, or null (with `error` set) if this run can't.
Run* parkableRun(Context& ctx, const char* name, IntrinsicResult& error) {
    Run* run = Run::of(ctx);
    if (!run || run->kind != Run::Kind::Request) {
        error = errorResult(std::string(name) + " can't be used at a page's top level");
        return nullptr;
    }
    return run;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

// Where a redirect may go: a path on this site, a relative URL, or an
// http(s) URL. No control characters or backslashes (browsers read '\' as
// '/'), and no "//host" (which leaves the site while looking like a path).
bool isAllowedRedirect(const std::string& url, std::string& why) {
    if (url.empty()) {
        why = "the URL is empty";
        return false;
    }
    for (unsigned char c : url) {
        if (c < 0x20 || c == 0x7F || c == '\\') {
            why = "the URL contains a control character or a backslash";
            return false;
        }
    }
    if (url.compare(0, 2, "//") == 0) {
        why = "a URL starting with // leaves the site; write https://host/... to mean that";
        return false;
    }
    size_t colon = url.find(':');
    size_t pathStart = url.find_first_of("/?#");
    if (colon != std::string::npos && (pathStart == std::string::npos || colon < pathStart)) {
        std::string scheme = url.substr(0, colon);
        for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (scheme != "http" && scheme != "https") {
            why = "only http and https URLs (or paths) are allowed";
            return false;
        }
    }
    return true;
}

void addRedirect() {
    Intrinsic f = Intrinsic::Create("redirect");
    f.AddParam("url");
    f.set_Code([](Context ctx, IntrinsicResult) {
        Value url = ctx.GetArg(0);
        if (!url.IsString()) return errorResult("redirect needs a URL string");
        std::string u = toStdString(url), why;
        if (!isAllowedRedirect(u, why)) return errorResult("redirect: " + why);
        OutcomeHandle h;
        h.kind = OutcomeHandle::Kind::Redirect;
        h.location = u;
        return IntrinsicResult(makeOutcomeHandle(std::move(h)));
    });
}

void addJson() {
    Intrinsic f = Intrinsic::Create("json");
    f.AddParam("value");
    f.set_Code([](Context ctx, IntrinsicResult) {
        std::string why;
        auto d = toDatum(ctx.GetArg(0), why);
        if (!d) return errorResult("json: the value " + why);
        OutcomeHandle h;
        h.kind = OutcomeHandle::Kind::Json;
        h.body = toJson(*d);
        return IntrinsicResult(makeOutcomeHandle(std::move(h)));
    });
}

void addStatus() {
    Intrinsic f = Intrinsic::Create("status");
    f.AddParam("code");
    f.AddParam("body", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult) {
        Value code = ctx.GetArg(0);
        Value body = ctx.GetArg(1);
        double n = code.IsNumber() ? code.DoubleValue() : 0;
        if (!(n >= 200 && n <= 599) || n != std::floor(n)) {
            return errorResult("status needs a status code from 200 to 599");
        }
        OutcomeHandle h;
        h.kind = OutcomeHandle::Kind::Status;
        h.status = static_cast<int>(n);
        if (body.IsString()) {
            h.body = toStdString(body);
            h.contentType = "text/plain; charset=utf-8";
        } else if (const OutcomeHandle* inner = outcomeHandleOf(body);
                   inner && inner->kind == OutcomeHandle::Kind::Json) {
            h.body = inner->body;
            h.contentType = "application/json";
        } else if (const Html* html = htmlHandleOf(body)) {
            h.body = html->str();
            h.contentType = "text/html; charset=utf-8";
            h.hasScript = html->hasScript();
        } else if (!body.IsNull()) {
            return errorResult("status's body must be a string, json(...), frag(...), or null");
        }
        return IntrinsicResult(makeOutcomeHandle(std::move(h)));
    });
}

// import "name": runs code/lib/name.ms (compiled with the app) and binds its
// top-level names, as a map, to `name`. Nothing else can be imported: the
// code partition is the only source of code (overview §1).
void addImport() {
    Intrinsic f = Intrinsic::Create("import");
    f.AddParam("libname");   // exactly one parameter: ManuallyPushCall requires it
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        if (partial.done) {
            Value arg = ctx.GetArg(0);
            std::string name = arg.IsString() ? toStdString(arg) : "";
            if (!isIdentifier(name)) {
                return errorResult("import needs the name of a file in code/lib/, like import \"util\"");
            }
            Run* run = Run::of(ctx);
            if (!run || !run->libs) return errorResult("import isn't available here");
            auto it = run->libs->find(name);
            if (it == run->libs->end()) return errorResult("import: there's no code/lib/" + name + ".ms");
            ctx.vm.ManuallyPushCall(ctx.baseIndex, it->second);
            return IntrinsicResult(arg, false);
        }
        // The module has run; its result is its locals map.
        Value module = ctx.vm.ManualCallResult;
        ctx.vm.SetVar(toStdString(partial.result).c_str(), module);
        return IntrinsicResult(module);
    });
}

// wait(seconds=1): parks the VM on a timer. The run's wall-clock budget
// still applies.
void replaceWait() {
    Intrinsic f = Intrinsic::GetByName("wait");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        if (!partial.done) return IntrinsicResult::Null;   // the timer fired
        IntrinsicResult error = IntrinsicResult::Null;
        Run* run = parkableRun(ctx, "wait", error);
        if (!run) return error;
        double seconds = ctx.GetArg(0).NumericVal();
        if (!(seconds > 0)) seconds = 0;
        double limit = static_cast<double>(run->host->options.budget.wallClock.count()) / 1000.0 + 1;
        if (seconds > limit) seconds = limit;   // the budget ends the run first anyway
        ScriptHost::Impl* host = run->host;
        uint64_t id = run->id;
        host->park(*run);
        host->loop->addTimer(std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)),
                             [host, id] { host->resume(id); });
        ctx.vm.yielding = true;
        return IntrinsicResult(Value::one, false);
    });
}

// Test intrinsics. Registered always (the registry is process-wide), but
// they work only in a host created with testIntrinsics.
Run* testRun(Context& ctx, const char* name, IntrinsicResult& error) {
    Run* run = parkableRun(ctx, name, error);
    if (run && !run->host->options.testIntrinsics) {
        error = errorResult(std::string(name) + " is only available in tests");
        return nullptr;
    }
    return run;
}

// _testSlowWork(ms, x): does `ms` milliseconds of "work" on the worker pool,
// with the VM parked, then returns x.
void addTestSlowWork() {
    Intrinsic f = Intrinsic::Create("_testSlowWork");
    f.AddParam("ms", Value::zero);
    f.AddParam("x", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        if (!partial.done) return IntrinsicResult(ctx.GetArg(1));   // the work is done
        IntrinsicResult error = IntrinsicResult::Null;
        Run* run = testRun(ctx, "_testSlowWork", error);
        if (!run) return error;
        double ms = ctx.GetArg(0).NumericVal();
        auto delay = std::chrono::milliseconds(ms > 0 ? static_cast<int64_t>(ms) : 0);
        ScriptHost::Impl* host = run->host;
        uint64_t id = run->id;
        host->park(*run);
        host->workers->submit([host, id, delay] {
            std::this_thread::sleep_for(delay);
            host->loop->post([host, id] { host->resume(id); });
        });
        ctx.vm.yielding = true;
        return IntrinsicResult(Value::one, false);
    });
}

// _testWithValue(x, f): parks on the worker pool, then calls f(x) and returns
// its result. This is the shape of file.update(path, @f) (M5): an intrinsic
// that waits, then calls back into script, without blocking the thread.
void addTestWithValue() {
    Intrinsic f = Intrinsic::Create("_testWithValue");
    f.AddParam("x", Value::Null);
    f.AddParam("f", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        if (partial.done) {
            IntrinsicResult error = IntrinsicResult::Null;
            Run* run = testRun(ctx, "_testWithValue", error);
            if (!run) return error;
            if (!ctx.GetArg(1).IsFuncRef()) return errorResult("_testWithValue needs a function");
            ScriptHost::Impl* host = run->host;
            uint64_t id = run->id;
            host->park(*run);
            host->workers->submit([host, id] { host->loop->post([host, id] { host->resume(id); }); });
            ctx.vm.yielding = true;
            return IntrinsicResult(Value::one, false);
        }
        if (partial.result.IsNumber() && partial.result.DoubleValue() == 1) {
            // Resumed: call f(x) through the stub. The stub reads these two
            // globals as its first instructions, before f runs, so a nested
            // call made by f can't disturb them.
            MiniScript::Interpreter interp = ctx.GetInterpreter();
            interp.SetGlobalValue("__ffCallback", ctx.GetArg(1));
            interp.SetGlobalValue("__ffCallbackArg", ctx.GetArg(0));
            ctx.vm.ManuallyPushCall(ctx.baseIndex, g_callbackStub);
            return IntrinsicResult(Value(2), false);
        }
        return IntrinsicResult(ctx.vm.ManualCallResult);   // f has returned
    });
}

FuncDef compileCallbackStub() {
    Value error;
    FuncDef stub = MiniScript::Interpreter::CompileToFunc("return __ffCallback(__ffCallbackArg)", "(fourfold)", &error);
    // Rooted for the life of the process.
    MiniScript::GCManager::AddRoot(Value::make_funcref(stub, Value::Null));
    return stub;
}

}  // namespace

void initMiniScriptRuntime(const ScriptHostOptions& options) {
    Value::MAX_COLLECTION_SIZE = options.budget.maxCollectionSize;
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    MiniScript::GCManager::Init();
    MiniScript::value_init_constants();
    ErrorTypes::Init();
    MiniScript::hostName = "Fourfold";
    MiniScript::hostVersion = versionString();
    Intrinsic::Count();   // registers the core intrinsics, so they can be replaced

    // Built-ins that don't belong in a request: they block the thread, reach
    // state shared by every VM, or run a process-wide garbage collection.
    disable("input", "there's no console to read from");
    disable("reset", "each request already starts fresh");
    disable("_in", "there's no REPL");
    disable("_out", "there's no REPL");
    disableGc();
    replaceWait();

    addRedirect();
    addJson();
    addStatus();
    addImport();
    addFrag();
    addMarkdown();
    addUnsafeHtml();
    addTestSlowWork();
    addTestWithValue();
    g_callbackStub = compileCallbackStub();

    // The built-in type maps (string, list, map, number, funcRef, error) are
    // shared by every VM in the process. Unfrozen, a script could add a method
    // to `string` and change every other request's behavior.
    MiniScript::CoreIntrinsics::StringType().Freeze();
    MiniScript::CoreIntrinsics::ListType().Freeze();
    MiniScript::CoreIntrinsics::MapType().Freeze();
    MiniScript::CoreIntrinsics::NumberType().Freeze();
    MiniScript::CoreIntrinsics::FunctionType().Freeze();
    MiniScript::CoreIntrinsics::ErrorType().Freeze();
    MiniScript::CoreIntrinsics::IntrinsicsMap();   // built frozen
}

void prepareInterpreter(Run& run, const MiniScript::List<MiniScript::FuncDef>& program) {
    run.interp = MiniScript::Interpreter::New();
    run.interp.set_hostData(&run);
    auto print = run.host->options.printOutput;
    std::string file = run.fileName;
    run.interp.set_standardOutput([print, file](String text, bool) {
        if (print) print(file, std::string(text.c_str(), static_cast<size_t>(text.LengthB())));
    });
    run.interp.Reset(program);
    // The engine creates these before the file runs (overview §4).
    run.interp.SetGlobalValue("get", Value::make_map(4));
    run.interp.SetGlobalValue("post", Value::make_map(4));
}

}  // namespace fourfold

// data: the app's data files (overview §3, Tier 1), over DataFiles.
//
//     data.get(name, default=null)      the value, a log as a list, or `default`
//     data.set(name, value)             replaces the value, all at once
//     data.append(name, value)          adds an entry to a log
//     data.update(name, @f, default=null)
//                                       f(old) returns the new value, with the
//                                       name locked meanwhile; returns it
//     data.increment(name, by=1)        adds to a number; returns the new one
//     data.compareAndSet(name, expected, new)
//                                       replaces the value only if it's `expected`
//     data.delete(name), data.exists(name), data.names(folder="")
//
// Failures come back as error values. All file work happens on the worker
// pool, with the VM parked; locks are waited for the same way.

#include <cmath>

#include "../bridge.h"
#include "../host_impl.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

// Partial results: where a multi-step call is.
constexpr double kWorking = 1;    // waiting for the worker pool (or a lock, then the pool)
constexpr double kCalling = 2;    // data.update: f is running
constexpr double kWriting = 3;    // data.update: writing f's result

// The run and its DataFiles, or null with `error` set.
Run* dataRun(Context& ctx, const char* function, DataFiles*& data, IntrinsicResult& error) {
    Run* run = Run::of(ctx);
    if (!run || run->kind != Run::Kind::Request) {
        error = intrinsicError(std::string("data.") + function + " can't be used at a page's top level");
        return nullptr;
    }
    data = run->context.data;
    if (!data) {
        error = intrinsicError(std::string("data.") + function + " isn't available here");
        return nullptr;
    }
    return run;
}

std::optional<RelPath> nameArg(Context& ctx, const char* function, IntrinsicResult& error) {
    Value v = ctx.GetArg(0);
    if (!v.IsString()) {
        error = intrinsicError(std::string("data.") + function + " needs a name string, like \"guestbook\"");
        return std::nullopt;
    }
    auto name = DataFiles::checkName(toStdString(v));
    if (!name) {
        error = intrinsicError(std::string("data.") + function + ": " + name.error().message);
        return std::nullopt;
    }
    return std::move(name).value();
}

std::optional<Datum> datumArg(Context& ctx, int index, const char* function, IntrinsicResult& error) {
    std::string why;
    std::optional<Datum> d = toDatum(ctx.GetArg(index), why);
    if (!d) {
        error = intrinsicError(std::string("data.") + function + ": the value " + why +
                               " (only numbers, strings, lists and maps can be stored)");
    }
    return d;
}

AsyncResult fail(const Error& e) {
    AsyncResult r;
    r.error = e;
    return r;
}

// Parks the run and does `work` on the worker pool.
IntrinsicResult startWork(Context& ctx, Run& run, std::function<AsyncResult()> work) {
    ScriptHost::Impl* host = run.host;
    uint64_t id = run.id;
    host->park(run);
    host->workers->submit([host, id, work = std::move(work)] { host->complete(id, work()); });
    ctx.vm.yielding = true;
    return IntrinsicResult(Value(kWorking), false);
}

// Parks the run, takes `name`'s lock, then does `work` on the worker pool
// and releases the lock. The lock is released even if the run has ended
// meanwhile.
IntrinsicResult startLockedWork(Context& ctx, Run& run, DataFiles* data, const RelPath& name,
                                std::function<AsyncResult()> work) {
    ScriptHost::Impl* host = run.host;
    uint64_t id = run.id;
    host->park(run);
    data->locks().acquire(DataFiles::lockKey(name),
                          [host, id, data, work = std::move(work)](Status locked, LockService::Ticket ticket) {
                              host->workers->submit([host, id, data, locked, ticket, work] {
                                  AsyncResult r = locked ? work() : fail(locked.error());
                                  data->locks().release(ticket);
                                  host->complete(id, std::move(r));
                              });
                          });
    ctx.vm.yielding = true;
    return IntrinsicResult(Value(kWorking), false);
}

// The finished work's result, as the intrinsic's return value.
IntrinsicResult finishWork(Run& run, const char* function) {
    std::optional<AsyncResult> result = std::move(run.async);
    run.async.reset();
    if (!result) return intrinsicError(std::string("data.") + function + ": no result (an engine bug)");
    if (result->error) return intrinsicError(std::string("data.") + function + ": " + result->error->message);
    return IntrinsicResult(toValue(result->value));
}

Intrinsic makeGet() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.AddParam("default", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "get", data, error);
        if (!run) return error;
        if (!partial.done) {
            if (run->async && !run->async->error && !run->async->found) {
                run->async.reset();
                return IntrinsicResult(ctx.GetArg(1));   // nothing stored: the default
            }
            return finishWork(*run, "get");
        }
        auto name = nameArg(ctx, "get", error);
        if (!name) return error;
        return startWork(ctx, *run, [data, n = *name] {
            auto v = data->get(n);
            if (!v) return fail(v.error());
            AsyncResult r;
            if (v.value()) r.value = *v.value();
            else r.found = false;
            return r;
        });
    });
    return f;
}

Intrinsic makeSet() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.AddParam("value", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "set", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "set");
        auto name = nameArg(ctx, "set", error);
        if (!name) return error;
        auto value = datumArg(ctx, 1, "set", error);
        if (!value) return error;
        // Under the name's lock, so a set can't land in the middle of
        // someone's update.
        return startLockedWork(ctx, *run, data, *name, [data, n = *name, v = *value] {
            Status s = data->set(n, v);
            return s ? AsyncResult{} : fail(s.error());
        });
    });
    return f;
}

Intrinsic makeAppend() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.AddParam("value", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "append", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "append");
        auto name = nameArg(ctx, "append", error);
        if (!name) return error;
        auto value = datumArg(ctx, 1, "append", error);
        if (!value) return error;
        // No lock: one appended line can't interleave with another.
        return startWork(ctx, *run, [data, n = *name, v = *value] {
            Status s = data->append(n, v);
            return s ? AsyncResult{} : fail(s.error());
        });
    });
    return f;
}

Intrinsic makeDelete() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "delete", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "delete");
        auto name = nameArg(ctx, "delete", error);
        if (!name) return error;
        return startLockedWork(ctx, *run, data, *name, [data, n = *name] {
            auto removed = data->remove(n);
            if (!removed) return fail(removed.error());
            AsyncResult r;
            r.value = Datum::number(removed.value() ? 1 : 0);
            return r;
        });
    });
    return f;
}

Intrinsic makeExists() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "exists", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "exists");
        auto name = nameArg(ctx, "exists", error);
        if (!name) return error;
        return startWork(ctx, *run, [data, n = *name] {
            auto e = data->exists(n);
            if (!e) return fail(e.error());
            AsyncResult r;
            r.value = Datum::number(e.value() ? 1 : 0);
            return r;
        });
    });
    return f;
}

Intrinsic makeNames() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("folder", Value(""));
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "names", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "names");
        Value v = ctx.GetArg(0);
        if (!v.IsString()) return intrinsicError("data.names needs a folder name string (\"\" for all of data/)");
        auto folder = DataFiles::checkFolder(toStdString(v));
        if (!folder) return intrinsicError("data.names: " + folder.error().message);
        return startWork(ctx, *run, [data, dir = std::move(folder).value()] {
            auto names = data->names(dir);
            if (!names) return fail(names.error());
            Datum::ListItems items;
            for (const std::string& n : names.value()) items.push_back(Datum::string(n));
            AsyncResult r;
            r.value = Datum::list(std::move(items));
            return r;
        });
    });
    return f;
}

Intrinsic makeIncrement() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.AddParam("by", Value::one);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "increment", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "increment");
        auto name = nameArg(ctx, "increment", error);
        if (!name) return error;
        Value by = ctx.GetArg(1);
        if (!by.IsNumber() || !std::isfinite(by.DoubleValue())) {
            return intrinsicError("data.increment's `by` must be a number");
        }
        return startLockedWork(ctx, *run, data, *name, [data, n = *name, amount = by.DoubleValue()] {
            auto now = data->increment(n, amount);
            if (!now) return fail(now.error());
            AsyncResult r;
            r.value = Datum::number(now.value());
            return r;
        });
    });
    return f;
}

Intrinsic makeCompareAndSet() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.AddParam("expected", Value::Null);
    f.AddParam("value", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "compareAndSet", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "compareAndSet");
        auto name = nameArg(ctx, "compareAndSet", error);
        if (!name) return error;
        auto expected = datumArg(ctx, 1, "compareAndSet", error);
        if (!expected) return error;
        auto value = datumArg(ctx, 2, "compareAndSet", error);
        if (!value) return error;
        return startLockedWork(ctx, *run, data, *name, [data, n = *name, e = *expected, v = *value] {
            auto swapped = data->compareAndSet(n, e, v);
            if (!swapped) return fail(swapped.error());
            AsyncResult r;
            r.value = Datum::number(swapped.value() ? 1 : 0);
            return r;
        });
    });
    return f;
}

// data.update(name, @f, default=null). Four steps, each a call of the
// intrinsic: take the lock and read (parked); call f(old) through the
// callback stub; write f's result (parked), which releases the lock; return
// it. While f runs, the lock is held by the run, and released if the run
// ends; nested updates of other names are fine, of the same name an error.
Intrinsic makeUpdate() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.AddParam("f", Value::Null);
    f.AddParam("default", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "update", data, error);
        if (!run) return error;
        double step = partial.done ? 0 : partial.result.DoubleValue();

        if (step == 0) {
            auto name = nameArg(ctx, "update", error);
            if (!name) return error;
            if (!ctx.GetArg(1).IsFuncRef()) {
                return intrinsicError("data.update needs a function: data.update(\"" + name->str() + "\", @f)");
            }
            if (run->dataLocks.count(name->str())) {
                return intrinsicError("data.update(\"" + name->str() + "\") inside an update of the same name "
                                      "would wait for itself");
            }
            ScriptHost::Impl* host = run->host;
            uint64_t id = run->id;
            host->park(*run);
            run->updates.push_back(*name);
            // Recorded now, so that if the run ends while waiting, finish()
            // gives the ticket up (or releases the lock).
            run->dataLocks[name->str()] = data->locks().acquire(
                DataFiles::lockKey(*name), [host, id, data, n = *name](Status locked, LockService::Ticket) {
                    host->workers->submit([host, id, data, n, locked] {
                        if (!locked) return host->complete(id, fail(locked.error()));
                        auto v = data->get(n);
                        if (!v) return host->complete(id, fail(v.error()));
                        AsyncResult r;
                        if (v.value()) r.value = *v.value();
                        else r.found = false;
                        host->complete(id, std::move(r));
                    });
                });
            ctx.vm.yielding = true;
            return IntrinsicResult(Value(kWorking), false);
        }

        if (step == kWorking) {
            // Read (or failed). Call f(old) through the stub.
            std::optional<AsyncResult> result = std::move(run->async);
            run->async.reset();
            RelPath name = run->updates.back();
            if (!result || result->error) {
                run->updates.pop_back();
                data->locks().release(run->dataLocks[name.str()]);
                run->dataLocks.erase(name.str());
                return intrinsicError("data.update: " + (result ? result->error->message : std::string("no result")));
            }
            Value old = result->found ? toValue(result->value) : ctx.GetArg(2);
            MiniScript::Interpreter interp = ctx.GetInterpreter();
            interp.SetGlobalValue("__ffCallback", ctx.GetArg(1));
            interp.SetGlobalValue("__ffCallbackArg", old);
            // The pushed frame reuses this call's argument registers; nothing
            // after this reads the arguments, only run->updates.
            ctx.vm.ManuallyPushCall(ctx.baseIndex, callbackStub());
            return IntrinsicResult(Value(kCalling), false);
        }

        if (step == kCalling) {
            // f has returned. Write its result, then let go of the lock.
            Value returned = ctx.vm.ManualCallResult;
            RelPath name = run->updates.back();
            run->updates.pop_back();
            LockService::Ticket ticket = run->dataLocks[name.str()];
            run->dataLocks.erase(name.str());
            if (returned.IsError()) {
                data->locks().release(ticket);
                return IntrinsicResult(returned);   // f's error, unchanged; nothing written
            }
            std::string why;
            std::optional<Datum> value = toDatum(returned, why);
            if (!value) {
                data->locks().release(ticket);
                return intrinsicError("data.update: f's result " + why +
                                      " (only numbers, strings, lists and maps can be stored)");
            }
            // The writer releases the lock, after the write; so even if this
            // run ends now, no one else reads before the write lands.
            ScriptHost::Impl* host = run->host;
            uint64_t id = run->id;
            host->park(*run);
            host->workers->submit([host, id, data, name, ticket, v = *value] {
                Status s = data->set(name, v);
                data->locks().release(ticket);
                AsyncResult r;
                if (!s) r.error = s.error();
                else r.value = v;
                host->complete(id, std::move(r));
            });
            ctx.vm.yielding = true;
            return IntrinsicResult(Value(kWriting), false);
        }

        return finishWork(*run, "update");   // kWriting: the write is done
    });
    return f;
}

}  // namespace

void addData() {
    addModule("data", {
                          {"get", makeGet()},
                          {"set", makeSet()},
                          {"append", makeAppend()},
                          {"update", makeUpdate()},
                          {"increment", makeIncrement()},
                          {"compareAndSet", makeCompareAndSet()},
                          {"delete", makeDelete()},
                          {"exists", makeExists()},
                          {"names", makeNames()},
                      });
}

}  // namespace fourfold

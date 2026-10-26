#include "data_work.h"

#include "../bridge.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

Run* dataRun(Context& ctx, const std::string& function, DataFiles*& data, IntrinsicResult& error) {
    Run* run = Run::of(ctx);
    if (!run || run->kind != Run::Kind::Request) {
        error = intrinsicError(function + " can't be used at a page's top level");
        return nullptr;
    }
    data = run->context.data;
    if (!data) {
        error = intrinsicError(function + " isn't available here");
        return nullptr;
    }
    return run;
}

AsyncResult fail(const Error& e) {
    AsyncResult r;
    r.error = e;
    return r;
}

IntrinsicResult startWork(Context& ctx, Run& run, std::function<AsyncResult()> work) {
    ScriptHost::Impl* host = run.host;
    uint64_t id = run.id;
    host->park(run);
    host->workers->submit([host, id, work = std::move(work)] { host->complete(id, work()); });
    ctx.vm.yielding = true;
    return IntrinsicResult(Value(kWorking), false);
}

IntrinsicResult startLockedWork(Context& ctx, Run& run, DataFiles* data, std::string lockKey,
                                std::function<AsyncResult()> work) {
    ScriptHost::Impl* host = run.host;
    uint64_t id = run.id;
    host->park(run);
    data->locks().acquire(lockKey,
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

IntrinsicResult finishWork(Run& run, const std::string& function) {
    std::optional<AsyncResult> result = std::move(run.async);
    run.async.reset();
    if (!result) return intrinsicError(function + ": no result (an engine bug)");
    if (result->error) return intrinsicError(function + ": " + result->error->message);
    return IntrinsicResult(toValue(result->value));
}

}  // namespace fourfold

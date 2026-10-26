// What the data and file modules share: finding the run's DataFiles, and
// doing file work on the worker pool with the VM parked (optionally holding
// a file's lock). Each intrinsic is called again when the work is done, and
// finishes with finishWork.
#pragma once

#include <functional>
#include <string>

#include "../host_impl.h"
#include "appdata/data_files.h"

namespace fourfold {

// The partial result of an intrinsic waiting on the worker pool (or a lock,
// then the pool).
constexpr double kWorking = 1;

// The run and its DataFiles, or null with `error` set. `function` is the
// name for messages ("data.get").
Run* dataRun(MiniScript::Context& ctx, const std::string& function, DataFiles*& data,
             MiniScript::IntrinsicResult& error);

AsyncResult fail(const Error& e);

// Parks the run and does `work` on the worker pool.
MiniScript::IntrinsicResult startWork(MiniScript::Context& ctx, Run& run, std::function<AsyncResult()> work);

// Parks the run, takes the lock `lockKey`, then does `work` on the worker
// pool and releases the lock. The lock is released even if the run has
// ended meanwhile.
MiniScript::IntrinsicResult startLockedWork(MiniScript::Context& ctx, Run& run, DataFiles* data,
                                            std::string lockKey, std::function<AsyncResult()> work);

// The finished work's result (its value), as the intrinsic's return value.
MiniScript::IntrinsicResult finishWork(Run& run, const std::string& function);

}  // namespace fourfold

# Suggested changes to MiniScript 2

*Started 2026-10-24, after M2. A running list of changes to MiniScript 2 (`../miniscript2`) that would help Fourfold. Each entry gives the problem, a suggested shape for the change, Fourfold's current workaround, and what the change would buy us. The shapes are suggestions; the MS2 side knows best how to fit them in. Background and measurements are in [design/m2-results.md](design/m2-results.md) and [design/m3-results.md](design/m3-results.md).*

File references are to MS2's C# sources (`cs/`), which the C++ in `generated/` is transpiled from.

## Summary

| # | Change | Priority | Workaround today |
|---|---|---|---|
| 1 | Per-VM memory accounting and limit | **High** (M5 shipped `fourfold serve` without it) | None adequate |
| 2 | An async-safe "call this function with these arguments" for intrinsics | High (`data.update` uses the workaround) | A stub that reads two globals |
| 3 | `Run` reports the instructions it executed | High | Count whole slices |
| 4 | Intrinsics can charge extra instructions | Medium | None |
| 5 | Built-in type maps per VM (or copy-on-write) | Medium | Frozen for everyone |
| 6 | Intrinsic sets per VM | Medium (M6) | Process-wide replacement, plus checks inside each intrinsic |
| 7 | Cheaper, configurable VM construction | Medium | None needed yet |
| 8 | Binary-safe strings (NUL bytes) | Medium (M7, M9) | NUL becomes U+FFFD |
| 9 | A public compile API returning a rooted program | Low | Our own copy of the compile pipeline, plus manual rooting |
| 10 | `Interpreter` releases its globals when destroyed | Low (easy) | Always call `Reset()` |
| 11 | `Globals` id counter that can't overflow | Low (easy) | None |
| 12 | `RunFunction` fails cleanly if the callee yields or waits | Low | Never use it |
| 13 | Not-done intrinsics yield by default | Low | Fourfold's intrinsics set `yielding` themselves |
| 14 | Bytecode serialization | Low (M5: not needed for v1) | Compile only the requested page |
| 15 | A GC per thread | Later (M11) | Several processes |
| 16 | Resumable compiling (one file in slices) | Low | Compile a file at a time |

---

## 1. Per-VM memory accounting and limit

**Problem.** The GC counts objects, not bytes, and doesn't know which VM allocated what. Within its instruction budget, one request can allocate far more memory than the machine has. In M2's measurement, a loop keeping 1 MB strings reached about 7.6 GB before the 10-second wall-clock budget stopped it. `Value.MAX_COLLECTION_SIZE` limits the size of one string or list, not how many a script keeps.

**Suggested shape.**
- Each VM gets an allocation counter and an optional limit, e.g. `vm.AllocatedBytes` and `vm.MemoryLimit`.
- Allocation sites (strings, lists, maps, and their growth) charge the running VM. `VM.ActiveVM()` is already thread-local and set during `Run`, so the sites can find it without new plumbing.
- Going over the limit raises a runtime error ("out of memory: this script used more than N MB") in that VM only.
- A first version could count bytes *allocated* (cumulative, like an instruction budget), which is simple and deterministic. Counting *live* bytes is more precise, but needs the GC to credit frees back to their VM.

**Workaround today.** None that's adequate:
- `fourfold dev` listens only on localhost (but since M5, `fourfold serve` doesn't, by decision; it says at startup that there's no per-request memory limit);
- the wall-clock budget bounds how long a hog can grow;
- `MAX_COLLECTION_SIZE` (set to 16 M) stops single huge allocations.

**Benefit.**
- Resolves risk R4, and lets Fourfold honor the per-request memory budget promised in the overview (§2, "Resource budgets").
- The request at fault gets a friendly error, and every other request carries on.
- The alternative, a process-level memory limit alone, kills every request in flight in that process. That turns one hostile request into a denial-of-service tool, and would pull the multi-process model forward into M5.
- With this change, the process limit becomes only a backstop, and the multi-process model can stay in M11.
- The dev tools' request inspector could show each request's memory use (overview §9).

## 2. An async-safe way for an intrinsic to call a script function with arguments

**Problem.** There are two ways for an intrinsic to run script code today, and neither does this job:
- `VM.RunFunction(funcRef, args)` runs the callee to completion re-entrantly. It ignores time slices and budgets, loops forever if the callee yields, and goes wrong if the callee calls an intrinsic that waits (see #12).
- `VM.ManuallyPushCall(baseIndex, funcDef)` works with parking and slicing, but runs a `FuncDef` with no arguments and no closure. It also assumes the calling intrinsic has exactly one parameter, since the pushed frame starts at `baseIndex + 2` (it was built for `import`).

**Suggested shape.** Something like `vm.PushCall(ctx.baseIndex, funcRef, args)`. Called from an intrinsic, it would push a call to any function value (closures included, binding `self` if it's a method), with the given arguments, and regardless of the intrinsic's own parameter count. The intrinsic returns a not-done result, and is re-invoked with the callee's return value (e.g. in `vm.ManualCallResult`) once the callee returns. The callee may yield and park like any other code, and its instructions count against the same slice.

**Workaround today.** Fourfold compiles a stub once: `return __ffCallback(__ffCallbackArg)`.
- To call `f(x)`, the intrinsic sets those two globals and pushes the stub with `ManuallyPushCall`. Then it reads `ManualCallResult` when re-invoked.
- This relies on the stub reading the globals as its first instructions, before a nested call can overwrite them.
- It puts two engine names into the app's global namespace.
- It works around the one-parameter assumption only because the intrinsic no longer needs its arguments once the call is pushed.

**Benefit.**
- `file.update(path, @f)` (M5, overview §3) and any later API that takes a callback become straightforward and robust.
- Engine names stay out of app globals.
- No reliance on evaluation order inside a stub.
- The same mechanism could replace Fourfold's handler stubs (`_ff_result = render(_ff_req)`), letting the engine call `render` and `get.x` directly, with no stub per handler.

## 3. `Run` reports the instructions it executed

**Problem.** `VM.Run(maxCycles)` stops after at most `maxCycles` instructions, but also earlier, on a yield, a pending intrinsic, or the end of the program. It doesn't say how many it actually ran.

**Suggested shape.** Return the count, or keep a running total on the VM (`vm.CyclesExecuted`).

**Workaround today.** Fourfold charges every slice its full size (10,000 instructions). A run can therefore be stopped up to one slice early, and its reported usage is an overestimate, more so for code that waits often.

**Benefit.**
- Exact instruction budgets.
- Accurate per-request instruction counts for the request inspector (overview §9).
- Fairer scheduling between VMs.

## 4. Intrinsics can charge extra instructions

**Problem.** Every intrinsic call costs one instruction, however much work it does: `sort` on a large list, `split`, `replace`, string repetition.

**Suggested shape.** `ctx.vm.ChargeCycles(n)` for an intrinsic to add to the current run's count (and so to its slice), in proportion to its work. Built-ins with work that grows with their input would call it. Fourfold's own intrinsics could too.

**Workaround today.** None. The wall-clock budget is the only limit on a script that does most of its work inside built-ins.

**Benefit.** Instruction budgets that reflect real work, so an endless loop of `sort` calls is stopped as promptly as an endless loop of arithmetic, and the scheduler's slices stay roughly equal in time.

## 5. Built-in type maps per VM

**Problem.** The `string`, `list`, `map`, `number`, `funcRef` and `error` type maps are process-wide and writable by design (`notes/FROZEN_VALUES.md`). In a server, a script that adds `string.shout` changes the behavior of every other request running at the same time, and of every later one.

**Suggested shape.** Either of these:
- Each VM (or each `Globals`) gets its own type maps, which inherit from frozen process-wide originals. A script's additions then live and die with its VM.
- A host option that makes the built-in maps copy-on-write per VM.

**Workaround today.** Fourfold freezes all six maps at startup, so adding a method to a built-in type is a runtime error for every script.

**Benefit.**
- Restores a normal MiniScript feature (extending built-in types) for Fourfold apps, without breaking isolation between requests.
- Removes a class of cross-request interference by construction, not by restriction.

## 6. Intrinsic sets per VM

**Problem.** The intrinsic registry is process-wide (`Intrinsic._all`). Each VM copies all of it at `Reset`. There's no way to remove an intrinsic, and replacing one works only before it's first built (`EnsureBuilt` copies the code once).

**Suggested shape.** Let the host give `VM.Reset` (or the `Interpreter`) the set of intrinsics this VM should have. That could be a filter, an explicit table, or "the core set plus these, minus those". The `intrinsics` map would then list only what the VM has.

**Workaround today.**
- Fourfold never calls `ShellIntrinsics.Init`.
- It replaces `input`, `reset`, `_in`, `_out`, `gc` and `wait` process-wide, before the first VM is built.
- Its own intrinsics are registered for every VM, and each checks the run's kind when called (for example, `wait` refuses to work at a page's top level during compiling, and the `_test*` intrinsics refuse unless the host enabled them).

**Benefit.**
- Different kinds of runs need different modules (Arch §5, `RunContext`): requests, jobs, migrations (no `http`, `mail` or `enqueue`), and the restricted VM that evaluates `access.ms` and config files as data (M6, `DataEvaluator`). With per-VM sets, these restrictions come from what each VM is given, as the architecture intends, instead of checks scattered through the intrinsics.
- An intrinsic that isn't there can't be reached at all, which is easier to trust than one that refuses.

## 7. Cheaper, configurable VM construction

**Problem.**
- `Interpreter.Reset(functions)` always creates `VM.New()` with the default sizes. That preallocates 10,240 stack slots, 10,240 name slots (about 160 KB), and 256 call frames.
- It registers a GC mark callback, which is O(n) to remove later.
- It builds a per-VM table of every intrinsic.

M2 measured about 0.1 ms per fresh VM, most of it here.

**Suggested shape.** Either or both of these:
- Let `Interpreter.Reset` (or `Interpreter.New`) take the VM's stack and call-stack sizes.
- Allow a VM to be recycled: reset to a new program with fresh globals, reusing its allocated stacks and intrinsic table.

**Workaround today.** None needed yet: the cost is within M2's target of 0.5 ms per run.

**Benefit.**
- Lower per-request latency.
- More importantly, less memory per waiting request: 1,000 requests parked on slow I/O hold about 160 MB in VM stacks today, mostly unused.
- Smaller stacks also give a tighter recursion limit, which is fine for request handlers.

## 8. Binary-safe strings

**Problem.** MiniScript strings end at a NUL byte, so text with NULs can't pass through intact.

**Suggested shape.** Strings that carry an explicit length throughout, so any byte sequence that's valid UTF-8 (including U+0000) round-trips. The alternative is a documented binary type for byte data (perhaps the same RawData used by command-line MiniScript and raylib-miniscript).

**Workaround today.** Fourfold's bridge replaces each NUL with U+FFFD on the way into a script.

**Benefit.**
- Form fields and query values arrive exactly as sent.
- In M9, webhook handlers need `req.rawBody` byte-for-byte to check signatures (overview §6). A NUL anywhere in the body would break the check.
- Uploads (M7) are handled by the engine as handles, but any API that exposes their bytes would hit the same limit.

## 9. A public compile API returning a rooted program

**Problem.** `InterpreterStorage.Compile` keeps its result in the protected `compiledFunctions`. A host that compiles once and runs many VMs must copy the parse → simplify → code-generate pipeline itself. The compiled functions' constants (nested function templates, long strings) are GC values, but nothing roots them while no VM is running them. They're swept between runs unless the host roots them.

**Suggested shape.** A static `Interpreter.CompileProgram(source, fileName, out error)` returning a small `CompiledProgram` object, the counterpart of `CompileToFunc` for programs. The object holds the `List<FuncDef>`, keeps their constants rooted while it lives, and can be handed to `Interpreter.Reset`.

**Workaround today.**
- `src/script/compile.cpp` has its own copy of the pipeline.
- `ScriptBundle` roots each program's `@main` with `Value.make_funcref` plus `GCManager.AddRoot`, and unroots it when the bundle is released.

**Benefit.**
- One less copy of MS2 internals in Fourfold, which would otherwise drift if MS2's pipeline gains a step.
- The rooting rule stops being a trap for every host that caches compiled code.

## 10. `Interpreter` releases its globals when destroyed

**Problem.** `~InterpreterStorage` is the default destructor. An `Interpreter` dropped without calling `Reset()` leaves its globals map registered as a GC root forever, along with everything reachable from it.

**Suggested shape.** Release the globals (as `Reset` does) in the destructor.

**Workaround today.** Fourfold calls `interp.Reset()` on every path that ends a run, and a test checks for leaks.

**Benefit.** Removes a silent leak that any host can fall into, especially on error paths.

## 11. A `Globals` id counter that can't overflow

**Problem.** `Globals._lastId` is a signed 32-bit counter, incremented for every new namespace, with no wraparound handling. Signed overflow is undefined behavior in the C++ build. Fourfold creates at least one namespace per request, so at 1,000 requests a second it overflows in about 25 days.

**Suggested shape.** A 64-bit counter, or an unsigned one whose wraparound is harmless. The id is used to validate `FuncDef` global-slot caches, so a 32-bit wrap could in principle reuse an id still cached somewhere; 64 bits avoids that.

**Workaround today.** None.

**Benefit.** A long-running server stays correct indefinitely, which matters from M11 (production).

## 12. `RunFunction` fails cleanly if the callee yields or waits

**Problem.** `VM.RunFunction`:
- loops forever if the callee calls `yield`;
- if the callee calls an intrinsic that goes pending (such as `wait`), the pending state is mistaken for an `import`, the call returns early with a partial result, and a stale continuation fires later.

**Suggested shape.** Detect both cases and raise a runtime error ("can't wait inside a callback"), with the VM left consistent. #2 is the right tool for callbacks that may wait.

**Workaround today.** Fourfold never uses `RunFunction`.

**Benefit.** Turns a hang or corruption into a clear error for any host, including future Fourfold code that might reach for it.

## 13. Not-done intrinsics yield by default

**Problem.** When an intrinsic returns a not-done result without setting `vm.yielding`, `RunUntilDone` keeps re-invoking it until the time slice is used up. The built-in `wait` does this, so a waiting script burns its whole slice.

**Suggested shape.** Have `Run` return to the host after an intrinsic goes pending, as if it had yielded. The alternative is a flag on the intrinsic saying it should.

**Workaround today.** Fourfold replaced `wait`, and each of its async intrinsics sets `vm.yielding = true` itself.

**Benefit.** One less rule for every async intrinsic to remember. Forgetting it is a silent performance bug, not a visible failure.

## 14. Bytecode serialization

**Problem.** Compiled programs exist only in memory.

**Suggested shape.** Save a compiled program (functions and their constants) to bytes, and load it back.

**Workaround today.** Recompile. Since M3, compiling runs on the script thread, a file at a time, between requests; measured, that's good enough (m3-results.md). For CGI mode, M5 measured cold starts (m5-results.md): compiling the whole app per request costs about 3.4 ms per 100-line page, too slow for a large app, but CGI mode can cache everything else and compile only the requested page's file and its imports, a few milliseconds. So this isn't needed for v1.

**Benefit.**
- Compiling could run in a separate process that hands back a serialized compiled app, isolating long compiles from serving. M3 didn't need this.
- In CGI mode (M10), a request could load the requested page's bytecode instead of compiling it: a few milliseconds saved per request.

## 15. A GC per thread

**Problem.** The GC's state is process-global and unsynchronized, so every `Value` in a process must live on one thread.

**Suggested shape.** Make the GC's sets, roots and intern table per thread (`thread_local`), with values never shared between threads.

**Workaround today.** One script thread per process. Using more cores means running several engine processes (planned for M11).

**Benefit.**
- Several script threads in one engine process (Arch §3, "Using more than one core"): simpler deployment, shared caches, and SSE `publish` without fan-out between processes.
- A separate GC heap for compiling, so a large compile needn't share the request thread (another of M3's options, not needed so far).

## 16. Resumable compiling

**Problem.** Parsing and generating code for one file is a single call. Fourfold compiles on the script thread a file at a time, so a request arriving during a compile waits for the current file: about 20 µs per line, measured in M3. Files of normal size cost a few milliseconds, but a 5,000-line file would stall requests for about 0.1 s.

**Suggested shape.** A parser and code generator that can stop after a budget of work and continue later, like `Run(maxCycles)`.

**Workaround today.** One file per step. Only very large files are a problem, and in dev mode only for the developer who saved one.

**Benefit.** A request's worst delay during a compile no longer depends on the largest file in the app.

# Fourfold: M2 Results — Measurements and Decisions

*Status: 2026-10-24, at the end of M2. Records what M2 measured and decided about risks R1–R4 ([milestones.md](milestones.md), Risk register), and how Fourfold works with MiniScript 2 as it stands. M2 made no MS2 changes, by decision. Every gap is worked around in the `script` component, or listed under [MS2 issues](#ms2-issues-for-later).*

## Summary

| Risk | Outcome |
|---|---|
| **R1**: a fresh VM per request is too slow | **Retired.** About 130–150 µs per run for a trivial `render`, start to finish, including the hop to the script thread. Over HTTP: 4,300–6,300 requests/s on one script thread. The execution model stands. |
| **R2**: interleaving and parking VMs on one thread | **Retired, with workarounds.** 1,000 VMs parked at once; the thread stays responsive. An intrinsic can wait and then call back into script, nested. Two MS2 gaps are worked around (see below). |
| **R3**: instruction budget; removing compiling intrinsics | **Retired, with caveats.** Budgets work (to within one slice). Compiling intrinsics are absent or replaced. The shared type maps are frozen. |
| **R4**: per-VM memory budgets | **Decision needed.** MS2 has no per-VM memory accounting, and one request can allocate gigabytes within its budget. **Recommendation: per-VM allocation accounting in MS2, plus a process-level limit as a backstop.** See [R4](#r4-memory-budgets). |

Measurements were taken on the development Mac (macOS 13.6, Apple clang 15), with the `release` preset (optimized, with debug info).

---

## R1: the cost of a fresh VM

### Script thread alone (`bench_script`)

`tests/bench/bench_script.cpp` runs handlers through `ScriptHost`'s public API: compiled once, run 20,000 times, each run in a fresh VM. The *trivial* page returns one field from `req`. The *typical* page imports a module, loops 20 times calling a helper, and returns two fields.

| Page | Latency, one at a time | Throughput, 1 in flight | Throughput, 100 in flight |
|---|---|---|---|
| trivial | 130–150 µs | 7,300–7,700 /s | 6,000–6,600 /s |
| typical | 240–300 µs | 3,200–3,800 /s | 5,600–6,000 /s |

Ranges are across runs. The spike that preceded the implementation timed the VM work alone, with no thread hops, at about 100 µs per run. Most of that is building the VM:

- `VMStorage` preallocates 10,240 stack slots and 10,240 name slots (about 160 KB) and 256 call frames.
- It builds a per-VM table of every intrinsic.

### Over HTTP (`ab -k`, 20,000 requests)

Against `fourfold dev tests/apps/scripts` (release build). The page is `/?name=Ada`: it imports a module, runs `render`, and fills a two-field template. Dev mode logs every request line to the terminal, which is included in these numbers.

| Clients | Requests/s | Mean latency | Median | 99th percentile |
|---|---|---|---|---|
| 1 | 4,340 | 0.23 ms | <1 ms | <1 ms |
| 100 | 6,350 | 15.8 ms | 15 ms | 25 ms |
| 1,000 | 4,420 | 226 ms | 225 ms | 295 ms |
| *static file, 1 client* | *9,030* | | | |
| *static file, 100 clients* | *14,000* | | | |

There were no failed requests at any level.

### Decision

**The target is fresh-VM overhead under 0.5 ms per run, and it is met with room to spare (about 0.15 ms).** Pooling VMs isn't needed. If profiling later asks for more, the cheapest wins are:

- smaller VM stacks (`VM::New(stackSlots, callSlots)`), which would also lower the recursion limit;
- reusing a `VMStorage` across runs, with fresh globals each time.

Both are internal to `ScriptHost`.

One curiosity: with 100 runs in flight, trivial pages run a little *slower* than one at a time, while typical pages run faster. A plausible cause is many live VM stacks competing for cache, plus MS2's global GC marking every live VM. It's worth a look if throughput ever matters, but not now.

---

## R2: interleaving and parking

How it works (all in `src/script/`):

- **The script thread** is an `EventLoop` of its own. Runnable VMs take turns, round-robin, one slice each: `vm.Run(sliceInstructions)`, 10,000 by default. Slices run in batches of about 2 ms, then the loop handles timers and posted work.
- **Parking.** An intrinsic that must wait marks its run as parked and sets `vm.yielding`. It returns a not-done `IntrinsicResult`, then submits work to the `WorkerPool` (or sets a timer). When the work is done, it posts back to the script loop, and the run is runnable again.
  - MS2 re-invokes the intrinsic with `partialResult.done == false`, which then returns the final value.
  - Worker tasks hold only a run's id, never a `Value` or a pointer to a run, so a run that times out meanwhile is simply gone when its work returns.
- **Calling the handler.** The page's top level runs first. Then the VM is reset onto a precompiled stub, `_ff_result = render(_ff_req)`, in the same namespace (`vm.Reset(stub, interp.GetGlobals())`).
  - This keeps time slicing and parking working inside handlers.
  - `Interpreter::RunFunction` would hang on `yield`, and misbehave if the handler parks.
- **Calling back into script from an intrinsic** (the shape of M5's `file.update(path, @f)`). After resuming, the intrinsic sets two globals (the function and its argument) and pushes a precompiled stub, `return __ffCallback(__ffCallbackArg)`, with `vm.ManuallyPushCall`.
  - It then reads `vm.ManualCallResult` when it's re-invoked.
  - The stub reads both globals as its first instructions, so a nested callback can't disturb them.
  - MS2 offers no other way for an intrinsic to call a function with arguments without blocking the VM.

What the tests show (`tests/script/script_host_test.cpp`):

- 1,000 VMs parked at once on worker-pool work. A trivial request among them is answered in well under 200 ms, and all 1,000 complete.
- 200 overlapping `wait 0.2` calls finish in under 2 seconds in total.
- Callbacks into script work once, nested two deep with parking at each level, and with closures.
- An endless loop doesn't delay a concurrent quick request by more than a few slices.
- Over HTTP, a page that waits a second doesn't hold up other pages (checked by hand with `examples/scripts`).

R2 is retired. The design of §3 of the architecture holds, with the stub-based workarounds above. Both workarounds would be simpler with an MS2 API for an asynchronous call of a function with arguments.

---

## R3: budgets and the sandbox

| Limit | How | Result |
|---|---|---|
| Instructions | Counted per slice, as calls × slice size. MS2 doesn't report how many instructions actually ran, so a run may stop up to one slice early, never late. | An endless loop ends with "used more than its budget of N instructions". |
| Wall-clock time | A timer per run on the script loop, counting time parked. | `wait 5` with a 200 ms budget ends in "took longer than its time budget". |
| Recursion | MS2's 256-frame call stack. | "Call stack overflow". |
| One huge string or list | `Value::MAX_COLLECTION_SIZE`, set to 16 M elements. | `"a" * 1e9` and `[0] * 1e9` give **error values**, not allocations. This is MS2's style: the script sees an error value. A handler that ignores it and returns something else succeeds. |

**Built-ins.** Fourfold never calls `ShellIntrinsics::Init`, so `file`, `exec`, `env`, `exit` and MS2's `import` don't exist. Of the core built-ins:

- `input` (blocks on stdin), `reset`, `_in`, `_out` and `gc.collect`/`gc.stats` return error values. `reset` and `gc.collect` would run a process-wide collection; `_in` and `_out` are lists shared by every VM.
- `wait` parks the VM on a timer, instead of spinning through its slice.
- `import` is Fourfold's own. It resolves names only among the precompiled files directly in `code/lib/`, and names must be identifiers (no `/`, `.`, or `..`).

**Shared type maps.** The `string`, `list`, `map`, `number`, `funcRef` and `error` maps are process-wide in MS2, and writable. Fourfold freezes them once, at startup, so `string.shout = ...` is a runtime error instead of a change to every other request. The `version` and `intrinsics` maps are already frozen.

**Other protections** in this milestone:

- `req` is deep-frozen.
- `redirect()`, `json()` and `status()` return handles (`Value::NewHandle`), checked against a table of live handles, so a script can't forge them.
- Redirects must be paths, relative URLs, or http(s) URLs, with no control characters, backslashes, or a leading `//`.
- MS2 strings can't hold NUL bytes, so the bridge turns each NUL into U+FFFD.

R3 is retired. Remaining caveats are the slice-granular instruction count, and that one intrinsic call counts as one instruction however much work it does.

---

## R4: memory budgets

### What was measured

`bench_script` runs two hostile pages under the default budget: 10 M instructions and 10 s of wall-clock time.

| Page | Peak memory | Ended by |
|---|---|---|
| A list pushed in an endless loop | +30 MB or less (no visible rise in peak RSS) | the instruction budget |
| A loop keeping 1 MB strings (`parts.push(s + parts.len)`) | **about 7.6 GB** (peak RSS went from 0.2 GB to 7.85 GB) | the wall-clock budget, not the instruction budget |

Each 1 MB string costs one instruction. Under the instruction budget, a single request could allocate far more memory than the machine has. `MAX_COLLECTION_SIZE` limits one string's size, not how many a request keeps. The wall-clock budget is what stopped this run, and only because allocating gigabytes is slow.

### What MS2 offers

- Its GC counts objects, not bytes, and attributes nothing to a VM.
- Host code can see GC set sizes (`GCManager::Lists.LiveCount()`, etc.), and the process's memory use.
- Everything runs on the script thread, one slice at a time. So the memory a process gains during a slice belongs (roughly) to the run that slice was for.

### Options

1. **A process-level limit, restarting the worker when it's exceeded.** This needs the multi-process model.
   - Simple, and a good backstop in any case.
   - But on its own it's coarse: one hostile request takes down every request in flight in that process, which makes it a denial-of-service tool.
2. **Per-VM allocation accounting in MS2.** Allocation sites already know the running VM (`VM::ActiveVM()` is thread-local), so they could count bytes against it and raise "out of memory" past a limit.
   - This gives a friendly error for the guilty request only.
   - It needs an MS2 change.
3. **A host-side heuristic.** Sample the process's resident size after each slice, and charge growth to that slice's run.
   - Possible without MS2 changes.
   - But imprecise (memory reuse, GC timing), and better as an early-warning signal than a limit.

### Recommendation (needs a decision)

**Option 2, with option 1 as the backstop.**

- Per-VM accounting is the only way to give the right error to the right request. Until it exists, the overview's per-request memory budget (§2) can't be honored.
- The process-level limit protects the machine whatever MS2 does.

Implications for the milestones:

- **If option 2 is chosen,** the MS2 change should land before M5 lifts the localhost-only restriction. The multi-process model stays in M11.
- **If option 1 alone is chosen,** milestones.md already says the multi-process model then moves into M5.

**Until then:** dev mode listens only on localhost, which limits this to the developer's own machine. The wall-clock budget bounds how long a hog can grow. And `MAX_COLLECTION_SIZE` stops single huge allocations.

---

## Other findings

- **HTTP/1.0 keep-alive (an M1 bug, fixed).** The server kept HTTP/1.0 keep-alive connections open without sending `Connection: keep-alive`, so 1.0 clients (such as `ab -k`) waited for a close that never came. It now sends the header (RFC 9112 §9.3), with a test.
- **The template stopgap** caught a case worth keeping for M4: `<{{tag}}>` is text to the HTML tokenizer, yet the value `script` would make a tag. A field right after `<` or `</` is a compile error.
- **Teardown.** Discarding an `Interpreter` without `interp.Reset()` leaves its globals rooted forever. Fourfold resets on every path that ends a run, and a test checks that 900 runs (including errors and parked runs) leave no maps, lists, or functions behind.
- **Rooting.** Compiled bytecode's constants (nested functions, long strings) are swept unless rooted. Each `ScriptBundle` roots everything it compiled, and unroots it on the script thread when the last request using that generation lets go. The bundle's `shared_ptr` deleter posts itself to the script loop. A test checks that releasing a compiled app frees its functions.
- **MS2 details that tripped up test scripts**:
  - `self` and `outer` are keywords;
  - maps have no `get(key, default)` (use `hasIndex`);
  - operations that fail return error values rather than stopping the script.

---

## MS2 issues for later

None blocks M3–M4. The first two matter for M5.

| Issue | Why it matters | Current workaround |
|---|---|---|
| Per-VM memory accounting | R4: one request can exhaust the machine's memory | Localhost only; wall-clock budget; `MAX_COLLECTION_SIZE` |
| An asynchronous "call this function with these arguments" from an intrinsic | `file.update(path, @f)` and any callback API | `ManuallyPushCall` plus a stub reading two globals |
| `Run(maxCycles)` reporting the instructions it ran | Exact instruction budgets and accounting for the dev tools' request inspector | Count whole slices |
| Type maps per VM, or immutable built-ins | Isolation between requests | Frozen at startup (scripts can't extend `string` etc. at all) |
| `Interpreter` destruction releasing its globals | Leaks if a host forgets `Reset()` | Always `Reset()` |
| `Globals::_lastId` overflow (a signed 32-bit counter, undefined on overflow) | At 1,000 VMs/s it overflows after about 25 days of uptime | None yet; an MS2 fix is small |
| A clean way to remove intrinsics per VM | Different VM kinds (requests, jobs, migrations) need different modules (Arch §5) | Replace process-wide before the first VM; check the run's kind inside each intrinsic |
| `RunFunction` misbehaves if the callee yields or parks | A trap for any host | Never used; handlers run through stubs |

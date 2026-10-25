# Fourfold: M3 Results — Measurements and Decisions

*Status: 2026-10-25, at the end of M3. Records what M3 measured and decided about risk R5 ([milestones.md](milestones.md), Risk register), and how live recompiling works. Like M2, M3 made no MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| **R5**: can compiling share the script thread without stalling requests? | **Retired.** Compiling is sliced on the script thread, a file at a time. Recompiling a 500-page app takes about 1.1 s, and adds about 2 ms (median) and 5 ms (worst) to a request made meanwhile. |
| **R5**: can bytecode be kept per generation and released safely? | **Retired.** 200 swaps under load, with full collections between them, leave exactly one generation alive and no leaked objects, and run clean under ASan and UBSan. |
| Edit → save → see | About 0.5 s from saving a file to the browser's reload event (polling every 250 ms, 200 ms of quiet). The target was 2 s. |
| How `code/` is watched | **Polling**, through `CodeTree`. No OS file-event APIs. |

Measurements were taken on the development Mac (macOS 13.6, Apple clang 15), with the `release` preset unless noted.

---

## Where compiling runs

The architecture left three options open (Arch, Open questions): compile on the script thread in slices; compile in a separate process that returns a serialized compiled app; or a second GC heap, once MS2 has per-thread GCs. **M3 chose slices on the script thread.**

- A separate process needs bytecode serialization, which MS2 doesn't have yet.
- A second heap needs per-thread GCs in MS2.
- Slicing needs no MS2 changes, and the numbers below show it's good enough.

How it works (`src/script/compile.cpp`):

- `ScriptHost::compileScripts` keeps its blocking signature for its caller (the lifecycle thread, never the I/O loop).
- On the script thread, a `CompileJob` posts one task per file: each lib, then each page. Each task compiles one file and roots its bytecode in the bundle under construction before it ends, so a collection between tasks can't sweep it.
- A page's top level (which finds its handlers) runs as a **discovery run**, scheduled in slices by the same scheduler as requests. It has the same budgets as a request, and still can't park (`wait` at the top level is a compile error). When it ends, the job reads the page's handlers from its globals and posts the next file.
- The template and route passes are MS2-free, and run on the lifecycle thread.

### The cost (`bench_compile`)

`tests/bench/bench_compile.cpp` builds an app of 500 pages (each about 100 lines of MiniScript, with eight helpers, two handlers and a `render`, plus a template) and 20 libs. It recompiles the whole app while sending trivial requests, one at a time, and compares their latency with an idle server.

| | Requests | Median | 99th percentile | Max |
|---|---|---|---|---|
| Idle | 4,600–5,500 /s | 160–195 µs | 0.8–1.1 ms | 1.7–1.9 ms |
| During the compile | ~650 /s | 1.9–2.1 ms | 3.2–3.4 ms | 4.3–4.8 ms |

- The compile takes 1.13–1.18 s with requests in between, and 1.04–1.09 s alone: the requests cost it about 8%.
- For 100 pages: 0.24 s, with a worst request of 2.9 ms.
- A request waits for at most the file being compiled when it arrives, so the delay is about one file's compile time (here about 2 ms, or roughly 20 µs per line), not the whole compile's.

Had the whole compile been one task, as in M2, every request during it would have waited up to the full 1.1 s.

**Caveat.** The step is one file, so a very large file stalls requests for its whole compile: by the rate above, about 0.1 s for a 5,000-line file. Slicing within a file would need MS2's parser and code generator to be resumable. It isn't worth it now: in dev mode, the only one who waits is the developer who saved the file. It's listed under [MS2 issues](#ms2-issues-for-later) in case production apps turn out to have huge files.

---

## Generations

Each compile that succeeds makes a new `CompiledApp` with the next generation number. `AppInstance` swaps it into the `Engine` (`setApp`, under a mutex). Each request pins the app it started with (`shared_ptr<const CompiledApp>`), so a swap never changes the app under a request in progress.

The bytecode (`ScriptBundle`) belongs to its `CompiledApp`. As in M2, its `shared_ptr` deleter destroys it on the script thread, which unroots its bytecode. So an old generation goes away when the last request using it finishes, whichever thread that request finishes on.

### The stress test

`tests/engine/lifecycle/app_instance_test.cpp`, "swapping repeatedly under load":

- 200 iterations of editing a page and recompiling, while four client threads send requests, half of them to a handler that parks (`wait 0.01`).
- A full collection (`ScriptHost::stats(true)`) after every swap.
- At the end: every request was answered with a 200 and expected content. Exactly one bundle is alive (`ScriptHost::Stats::liveBundles`, new in M3). The live maps, lists and functions are back to the baseline taken before the swaps.
- At most 3 generations were alive at once: the current one, plus those still finishing parked requests.
- It also passes under the `asan` preset, as does the whole suite.

---

## Watching for changes

`AppInstance` (`src/engine/lifecycle/`) runs on a thread of its own, with its own `EventLoop`.

- **Polling.** Every `pollInterval` (250 ms), it lists `code/` through `CodeTree` and compares each file's path, size and modification time with the listing it last compiled.
- **Waiting for quiet.** A changed tree is compiled once two listings `quietPeriod` (200 ms) apart agree, so a burst of saves gives one compile.
- **`code/.deploy`.** Touching it compiles at once, without waiting for quiet. `CodeTree::deployTrigger()` is the one place the code partition looks at a dot-file. It's a stat with `AT_SYMLINK_NOFOLLOW`; a link or a folder counts as missing. `LifecycleOptions::Trigger::DeployOnly` ignores everything but the trigger, for later production use.
- **Failure.** A compile that fails keeps the old version serving. It isn't retried until the tree changes again.
- **Startup.** A first compile that fails no longer stops `fourfold dev`. The engine answers 503 with a short page until the code compiles, and the overlay shows the errors.

**Why polling.** It goes through `CodeTree`, so it follows the partition's rules (no symbolic links, no raw file APIs outside `partition`), and it's the same on every OS. It's also what CGI mode will need, since a CGI process has no time to watch anything: it checks for changes on each request (overview §8, Hosting modes). For an app of a few hundred files, a listing every 250 ms is cheap. FSEvents or inotify could cut the latency and the idle cost later, as an optimization behind `AppInstance`, if large apps call for it.

---

## Live reload and the overlay

`DevTools` (`src/devtools/`) is an `Observer` (it hears about compiles) and the engine's `EngineHooks` (it serves `/__dev/*`, decorates HTML responses, and makes the dev error page). The core includes neither its headers nor its library.

- **Injection.** Every `text/html` response gets `<script src="/__dev/client.js" data-generation="N" defer></script>` before its last `</body>`.
  - It's a file, not an inline script, so the CSP's `script-src 'self'` allows it without a nonce (nonces come in M4).
  - `N` is the generation that rendered the page.
- **Events.** `/__dev/reload` is a server-sent event stream, the first use of `HttpResponse::stream` (below).
  - `hello` on connecting: the current generation, plus the errors if the latest compile failed. A page whose generation differs reloads. This also covers a page that was open while the server restarted.
  - `reload` after a successful compile.
  - `css` when only existing stylesheets changed. The page re-points matching `<link rel=stylesheet>` elements, with `?__ff=N`, and doesn't reload.
  - `errors` after a failed compile.
  - A comment every 20 s keeps streams open, and finds closed ones.
- **The overlay.** For each error, it shows the file, line, message, and three lines of code either side, with the line highlighted.
  - The client builds it with `textContent` only, so nothing in a message or a line of code can become markup.
  - The JSON encoder also escapes `<`, `>` and `&`.
- **Runtime errors** in dev get a server-rendered HTML 500 page in the same style. Every piece goes through `escapeHtml`, and the page works without JavaScript. Production still gets `pages/500.html`.
- **Reserved paths.** The engine passes `/__dev/*` to the hooks *after* Admit, so the Host check still stops DNS-rebinding attacks on the dev tools. Without hooks (production), `/__dev/*` is a 404.

### Streamed responses

`HttpResponse` gained an optional `stream`: a function the transport calls, once the headers are sent, with a `StreamWriter`.

- `HttpServer` sends a streamed response without a `Content-Length`, with `Connection: close`.
- Streams are exempt from the request and idle timeouts.
- The writer holds the connection weakly, so a stream outliving its client just finds itself closed.
- A client that stops reading is dropped once 1 MB is waiting (`HttpServerLimits::maxStreamBufferBytes`).
- This is the start of what `push` will need in M9. The per-host connection limit of browsers (six on HTTP/1.1) means many dev tabs on one origin can run out; HTTP/2 (M11) removes that.

### End to end

`tests/hosts/cli/dev_server_test.cpp` runs a real `DevServer` on a scratch app, and checks each of these:

1. An edit to a `.ms` file gives a `reload` event (in about 0.5 s), and the new content.
2. An edit to a stylesheet gives a `css` event.
3. A syntax error gives an `errors` event with the code around it, while the old version keeps serving.
4. A page opened after that hears the errors in its `hello`.
5. Fixing the error gives a `reload`.

---

## Other changes

- **`Observer`** (`src/engine/observer/`): `compileStarted`, `compileFinished(CompileReport)`, `scriptError`. `ObserverList` fans events out; `DevServer` prints to the terminal as one observer, and `DevTools` is another. It replaces M2's `EngineOptions::onScriptError`.
- **The dev server's terminal output:**
  - diagnostics on every compile;
  - "Recompiled (pages/x.ms) in N ms" after each success;
  - "still serving the last version" after a failure;
  - no lines for the dev tools' own `/__dev/` requests.
- **Teardown order matters more now.** `DevServer` stops the HTTP server, then the app instance, then the script host (so no run can call back into the engine's options or hooks), and only then destroys the engine and dev tools.

---

## MS2 issues for later

In addition to [M2's list](m2-results.md#ms2-issues-for-later), kept with the rest in [../ms-source-changes.md](../ms-source-changes.md):

| Issue | Why it matters | Current workaround |
|---|---|---|
| Resumable parsing and code generation (compile one file in slices) | A very large `.ms` file stalls requests for its whole compile (about 20 µs per line) | Compile a file at a time; fine for files of normal size |
| Bytecode serialization | A compile in a separate process, and CGI mode's cached compiled app (M5 decides if it's needed) | Compile on the script thread |

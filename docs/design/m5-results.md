# Fourfold: M5 Results — State: Data and Sessions

*Status: 2026-10-26, at the end of M5. Records what M5 built and decided about risks R7 and R8 ([milestones.md](milestones.md), Risk register): data files, sessions and their locks, flash messages, CSRF protection, the engine log and the dropbox, `fourfold serve`, and the CGI spike. Like M2–M4, M5 made no MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| **R7**: are session locks and file locks awkward as scheduler waits? | **Retired.** A run that needs its session parks before its code starts, until the lock is granted and the session loaded; a `data` operation parks while it waits for its lock and does its I/O. No thread ever blocks on a lock held by a script. A run that ends (an error, its budget) while holding or awaiting a lock lets it go. |
| **R7**: is SQLite-backed state too slow for every request? | **Retired.** Loading and locking a session adds about 0.25 ms to a request, and saving it nothing measurable on top; throughput drops by 10–20% (see [Measurements](#measurements)). Most of the added time is the two hops to a worker thread and back. |
| **R8**: are CGI cold starts usable? | **Checked; yes, with a cache.** A cold start (open, compile, serve) takes about 5 ms for a small app, but compiling grows with the app: about 3.4 ms per 100-line page, so 340 ms for 100 pages. **Decision: bytecode serialization isn't needed for v1.** M10 caches the compiled app's plain parts and compiles, per request, only the requested page's `.ms` file and the libs it imports. Sessions and counters stay exact across 16 concurrent CGI processes. |
| The localhost-only restriction | **Lifted**, by `fourfold serve`. CSRF protection is complete. The per-request memory limit (R4) is still missing; that's an accepted risk, stated at startup (see [Serving](#fourfold-serve)). `fourfold dev` stays on localhost. |
| CSRF | A CSRF cookie separate from the session's; tokens are keyed BLAKE2b hashes of it, in every post form and in HTMX's headers. Requests other than GET, HEAD and OPTIONS need a matching token, and an `Origin` (if sent) of the site itself. |

Measurements were taken on the development Mac (macOS 13.6, Apple clang 15), with the `release` preset. The machine was noisier than in M2: `bench_script`'s trivial page ran at 155–210 µs per run (M2: 130–150 µs), with or without M5's new intrinsics registered, so the absolute numbers below are a little high; the differences between rows are what matter.

---

## Sessions

### What was built

- **`state.SessionStore`** (`src/state/`), over SQLite in `private/state/state.db` (WAL mode, a 5 s busy timeout). One row per session: the hash of its ID, the user (none until M6), the creation and last-use times, the visitor's address group, and the two parts, each as canonical JSON: the engine's (`{"flash": "…"}`) and the app's (the `session` map).
- **IDs** are 32 random bytes, sent as base64url in the cookie. The store keeps only their BLAKE2b hash, and a test checks that neither the ID nor its raw bytes appear anywhere in the database or its WAL.
- **Timeouts** as in the overview (24 h idle and 7 days in all for anonymous sessions; 14 and 30 days signed in), checked on every load. An hourly sweep deletes expired sessions. "Last used" is written at most every 5 minutes.
- **Caps:** 1,000 anonymous sessions per address group and 100,000 in all; creating one past either evicts the oldest anonymous sessions (of that group, or overall). An address group is an IPv4 address, or an IPv6 address's /64. Signed-in sessions are never evicted.
- **Cookies:** `__Host-ff_session` (`Secure`, `HttpOnly`, `SameSite=Lax`, `Path=/`) over HTTPS; `ff_session` without `Secure` in dev mode's plain HTTP. One setting decides (`EngineOptions::https`), not an `if (dev)`.

### Which requests load the session

The overview says a session is loaded and locked on first access to `session`. MS2 has no hook for a map's first access, so the decision is made at compile time, per route: **a route uses the session if its page's `.ms` file, or a lib it imports, names `session` or `flash`, or if its page template has a `{{flash}}` field.** The compiler finds the names by scanning each file's tokens. It skips comments and strings, and also `x.session`, since that's a member, not the global. It follows `import "name"` through libs, and assumes the worst if an import's argument isn't a string literal. `describe()` (the golden route listing) marks such routes `session`.

The effect is the same as the overview's for the common cases: static files, pages without code, and handlers that never mention the session don't touch the store or wait for a lock. A handler in a file that mentions `session` only on some paths still takes the lock; that's the price of deciding ahead of time.

### A request's session, step by step

1. **Identify** (`src/engine/pipeline/session.cpp`): the cookie's ID, if well formed, is hashed.
2. **Open**: `ScriptHost` starts the run parked and calls `SessionAccess::open` (an interface in `script`, implemented by the engine's `EngineSession`). That takes the session's lock from a `LockService`, then loads the record on a worker, then hands the run the `session` map (an ordinary, unfrozen MiniScript map) and the waiting flash message. The run's wall-clock budget covers the wait.
3. **Run**: the handler reads and changes `session` freely.
4. **Check** (still on the script thread): the `session` global goes through `toDatum` (the one inert check), and its canonical JSON must be within 64 KB. If it isn't a map, isn't inert, or is too big, the run's outcome becomes an error naming the problem (for one too big: its size, the limit, and its three largest keys with their sizes), and nothing is saved.
5. **Commit**, on a worker, before the response goes out: a new visitor whose map became non-empty (or who got a flash message) gets a new session and its cookie; an existing session is saved if either part changed, and otherwise just "touched"; then the lock is released. A save that fails turns the response into a 500.

**Flash messages.** `flash "…"` puts a message in the engine's part. The next page rendered with a `{{flash}}` field shows it (unless `render` filled `flash` itself), and it's cleared. A page with no code but a `{{flash}}` field also shows it: the engine loads the session without its lock, renders, and clears the message only if the engine part is still what it read (a compare-and-swap in SQLite), so it can't undo a concurrent change.

### Locks

`state.LockService` hands out named exclusive locks without blocking anyone: `acquire(key, granted)` returns a ticket at once, and `granted` is called when the lock is held. Waiters are served first come, first served. Every ticket is released exactly once, whether it was granted, failed, or given up while waiting. Two kinds:

- **in memory**, for one process: the standalone server's sessions;
- **in memory plus a lock file** (`flock`), for locks shared with other processes: data files, and sessions in CGI mode. Only the head of each key's queue waits on the file, on a worker thread, so a crowd of waiters can't use up the worker pool. (`DataFiles` gives its lock service two threads of its own for this.)

A stress test (200 grants and releases from many threads, both kinds) checks that no two holders ever overlap.

---

## Data files

`appdata.DataFiles` (`src/appdata/`) is Tier 1 of the overview's §3, and the script module `data` is its MiniScript face:

| Call | Does | Storage |
|---|---|---|
| `data.get(name, default=null)` | the value, a log as a list, or `default` if there's neither | reads `name.json`, else `name.jsonl` |
| `data.set(name, value)` | replaces the value, all at once | `name.json`: temporary file, `fsync`, rename |
| `data.append(name, value)` | adds an entry to a log | one line of `name.jsonl`, one `write` with `O_APPEND`; no lock |
| `data.update(name, @f, default=null)` | calls `f(old)` with the name locked, stores and returns what it returns | as `set` |
| `data.increment(name, by=1)` | adds to a number (missing counts as 0); returns the new one | as `set` |
| `data.compareAndSet(name, expected, value)` | replaces the value only if it equals `expected` (null matches missing); returns 1 or 0 | as `set` |
| `data.delete(name)`, `data.exists(name)`, `data.names(folder="")` | | |

- **Names** are partition paths without an extension (`"guestbook"`, `"polls/lunch"`), under `RelPath`'s rules: no `..`, no dot-names. Folders are made as needed. A name holds a value or a log, never both; using one kind of operation on the other is an error, not a conversion.
- **Everything is inert.** Values go in through `toDatum` and come out of `parseJson`, which refuses anything malformed, including a repeated key and invalid UTF-8. A damaged file is an error, not a guess. A log's last line without a newline (a crash part way through an append) is left out.
- **Locks.** `set`, `update`, `increment`, `compareAndSet` and `delete` hold the name's lock (in memory, plus `data/.fourfold/locks/<hash>.lock` for other processes), so a `set` can't land in the middle of someone's `update`. `get` and `append` take none: atomic renames mean a reader never sees half a file, and one-write appends don't interleave.
- **Errors are values**, as elsewhere in MiniScript: `data.get("../x")` returns an error saying why, which a handler can test with `isa error`.
- **A size cap** of 16 MB per value or log, a setting for M6's config.
- `data.update` is renamed from the overview's `file.update`: it works on data names, not files.

### `data.update`, the callback

`data.update` is the first real use of M2's "intrinsic calls back into script" pattern: it parks to take the lock and read; calls `f(old)` through the precompiled stub (`return __ffCallback(__ffCallbackArg)`, pushed with `ManuallyPushCall`); and parks again to write. Points worth knowing:

- The stub reuses the intrinsic's argument registers (MS2's `ManuallyPushCall` assumes a one-parameter intrinsic), so everything needed after `f` returns is kept in the run (`Run::updates`), not read from the arguments. This is MS2 issue #2 in [../ms-source-changes.md](../ms-source-changes.md), still worked around.
- If `f` returns an error, it's returned unchanged and nothing is written. If it returns something that isn't inert, that's an error too.
- The lock is held by the run while `f` runs, so nested updates of other names work, and an update of the same name inside its own `f` is an error ("would wait for itself") rather than a deadlock.
- If the run ends while holding the lock (its budget ran out inside `f`, say), the lock is released then. Once `f` has returned, the write's worker releases it, after the write, so a run killed at that moment can't let another request read the old value before the new one lands.

### JSON as the canonical form

The `Datum` codec is JSON (Arch §4's open question): `toJson` writes it canonically (sorted keys, the shortest exact numbers), `parseJson` reads it. M5 changed one thing in the writer: whole numbers up to 2^53 are written whole, so a counter's file says `400`, not `4e+02` (which reads back the same, but not to a person). JSON's `true` and `false` read as 1 and 0, MiniScript's booleans.

---

## CSRF

### Design, and a change from the overview

The overview put the CSRF token in the session's engine part. That would mean every visitor who loads a page with a form gets a session record, which undoes "bots that never write `session` cost nothing". So the token comes from a **separate CSRF cookie** instead:

- `__Host-ff_csrf` (or `ff_csrf` in dev mode): 32 random bytes, `HttpOnly`, `SameSite=Lax`, set on any HTML response to a visitor without a valid one. It costs nothing on the server: there's no record of it.
- The token is `base64url(BLAKE2b(key, "csrf:" + cookie))`, keyed with the engine's secret key (`private/state/secret.key`, 32 random bytes, made once; of several processes starting together, exactly one creates it).
- **Rendering** puts the token in every `<form method="post">` (a hidden `_csrf` field, at the spot M4's parser recorded) and on `<body>` as `hx-headers='{"X-CSRF-Token": "…"}'` for HTMX. An `hx-headers` written on `<body>` by the app is now a compile error.
- **The gate** (`policy.PolicyGate`, `src/engine/policy/`), for every method but GET, HEAD and OPTIONS, before any handler: an `Origin` header, if present, must be the site's own (`null` is refused); and the token, from the `_csrf` field or the `X-CSRF-Token` header, must match the cookie's, compared in constant time. A refusal is a 403 ("This form has expired"; in dev mode, the reason), logged to the engine log.
- **`GateApproval`** (Arch §7): `ScriptHost::run` now requires one, and only `PolicyGate` can make one. Tests use `TestGate` from `tests/support/`.

### What handlers see

`req` never contains the engine's cookies (`ff_*` and `__Host-ff_*` are removed from `req.headers.cookie`), the `X-CSRF-Token` header, or the `_csrf` field. So a handler that logs its headers can't leak a session ID. A test of this found a real bug: two `Cookie` header fields were merged with `, ` before the engine's cookies were removed, so `ff_csrf=…` hid inside another cookie's value and reached the app. Cookies are now filtered field by field, and joined with `; ` as RFC 6265 says.

---

## Logs and the dropbox

- **`state.EngineLog`** writes `private/state/logs/engine-YYYY-MM-DD.log` (UTC), one line per entry, from a background thread, so no request waits on the disk. Control characters in a message are escaped, so nothing can forge a line. It records startup, requests and compiles (in serve mode), script errors, CSRF refusals and failed session saves. It lives in `state` rather than `platform` (as Arch §5 had it), since it writes through `StateDir`, which is above `platform`. Rotation is M11's.
- **`dropbox.write(name, text)`** appends a line to `dropbox/<name>`; the `Dropbox` partition type offers only appending. In serve mode, `print` output goes to `dropbox/print.log`.

---

## `fourfold serve`

`fourfold serve <app> [--listen ADDRESS:PORT] [--host NAME]... [--plain-http]` runs the app for real use:

- no dev tools, and generic error pages;
- recompiles only when `code/.deploy` is touched;
- everything logged to the engine log;
- `--host` is required unless it listens on a loopback address, and requests for any other host are refused;
- Secure, `__Host-` cookies, and `https://` as the site's own origin, assuming a proxy in front that provides TLS. `--plain-http` turns that off.

`fourfold dev` and `fourfold serve` share one class, `AppServer` (renamed from M3's `DevServer`), which opens the stores and wires them into the engine.

At startup, `serve` says plainly what this build can't do yet: no TLS of its own, **no per-request memory limit**, and no sign-in, access rules or rate limits. The missing memory limit is R4, and m2-results recommended fixing it in MS2 (per-VM allocation accounting, [../ms-source-changes.md](../ms-source-changes.md) #1) before lifting the localhost restriction. M5 lifted it anyway, by decision: until that change lands, a hostile or buggy script can use as much memory as the machine has, bounded only by its time and instruction budgets.

---

## Measurements

### Sessions and data (`bench_state`)

`tests/bench/bench_state.cpp` sends requests through the engine (no HTTP) with the real SQLite store and data files, 5,000 per row. Each page fills a one-field template.

| Page | Latency, one at a time | Throughput, 1 in flight | Throughput, 50 in flight |
|---|---|---|---|
| plain `render` (no state) | 287 µs | 3,960 /s | 3,500 /s |
| reads `session` (lock, load) | 534 µs | 1,950 /s | 3,160 /s |
| writes `session` (lock, load, save) | 514 µs | 1,990 /s | 2,830 /s |
| `data.get` | 419 µs | 2,250 /s | 4,350 /s |
| `data.increment` (lock, read, atomic write) | 1,160 µs | 870 /s | 1,650 /s |

- A session costs about a quarter of a millisecond of latency, mostly thread hops: the load goes to a worker and back, and the commit goes to a worker before the response. With many requests in flight, those hops overlap, and the cost falls to 10–20% of throughput.
- **All 50 in flight use the same session**, so they queue on its lock. They still run at 2,830 /s: handing a lock from one run to the next is cheap.
- `data.increment` is dominated by the `fsync` of the temporary file and of the folder after the rename. That's the price of a counter that survives a crash. A cheaper counter (SQLite, in M8) is the answer if one is ever hot.

### Exactness

- In process (`tests/hosts/cli/state_test.cpp`): 8 clients × 25 requests on one `data.increment` counter, and then on one session from 8 "tabs" at once, over HTTP; the counts are exact. Afterwards, no lock is held or waited for, no run is left, and only the current app's bundle is alive.
- Across processes (the CGI spike, below): exact.

---

## The CGI spike (R8)

`fourfold cgi <app>` (unlisted) serves one request from the CGI environment: it opens the app's stores, compiles the app, runs the engine once with **file-lock** session locks, and writes the response.

**Cold starts** (release build; `FOURFOLD_CGI_TIMING=1` reports the steps):

| App | Open stores | Compile | Handle | In all | Wall clock |
|---|---|---|---|---|---|
| one `.ms` file | 2.2 ms | 1.2 ms | 1.1 ms | 4.7 ms | about 10 ms |
| `tests/apps/scripts` (4 `.ms` files, a lib) | 2.2 ms | 3.1 ms | 0.05 ms (404) | 5.4 ms | about 10 ms |

Compiling grows with the app (`bench_compile`, pages of about 100 lines, plus 20 libs): 91 ms for 20 pages, 340 ms for 100, 1.7 s for 500. So compiling the whole app per request is fine for a small app and unusable for a large one.

**Decision: bytecode serialization isn't needed for v1.** CGI mode (M10) will cache, in `private/state/`, everything in the compiled app that's plain C++ (routes, templates, Markdown pages, static files, which page has which handlers), and per request compile only the requested page's `.ms` file and the libs it imports: a few milliseconds. MS2 bytecode serialization ([../ms-source-changes.md](../ms-source-changes.md) #14) stays a possible later optimization.

**Across processes:** 16 concurrent loops of `fourfold cgi`, each running 25 counter increments and 25 increments of one shared session, three rounds: all 800 requests answered 200, the counter reached exactly 400, and the session exactly 402 (counting the first and last requests). The first try lost one request, to a 500: on macOS, `open` with `O_CREAT` can fail with `ENOENT` while another process creates the same file. Opening a lock file (and appending) now retries in that case.

---

## Other changes

- **Partitions** (`src/partition/`): `DataDir` (read, atomic write, append, list, delete, and lock files in `data/.fourfold/locks/`), `Dropbox` (append only) and `StateDir` (engine only; `create`, which fails if the file exists, for the secret key; and the host path SQLite needs). All go through one internal set of `openat`/`O_NOFOLLOW` helpers. `FileLock` wraps `flock`.
- **Foundation:** `parseJson`; `base/crypto.h` over **Monocypher** 4.0.2 (BLAKE2b, keyed BLAKE2b, constant-time comparison, base64url); an injectable `Clock`; `platform`'s `Sqlite` wrapper over **SQLite** 3.50.4 (both vendored in `third_party/`).
- **`RunContext`** (Arch §5) now exists: the request, the render options, and the services a run may use (`SessionAccess`, `DataFiles`, `Dropbox`). A module whose service is null reports that it isn't available. `script` now depends on `appdata` (for `DataFiles`), both being plain C++ below the engine.
- New intrinsic modules, one file each: `data`, `dropbox`, `flash`. A module is a zero-argument intrinsic returning a frozen map of unnamed intrinsics, as M2's `gc` replacement already was.
- **Samples:** `examples/guestbook` now stores its entries (`data.append`), seeds its sample entries exactly once (`data.compareAndSet`), counts visits (`data.increment`), keeps each visitor's own count in `session`, and thanks the signer with `flash`. The end-to-end tests now run the example apps from temporary copies, so they never write state into the source tree.

---

## MS2 issues

None new. Two from M2's list matter more now:

| Issue | Status after M5 |
|---|---|
| #1 Per-VM memory accounting | Still missing. `fourfold serve` runs without it, by decision, and says so. |
| #2 An async-safe "call this function with these arguments" | `data.update` uses the workaround (a stub reading two globals, with the intrinsic's state kept in the run). Works and is tested, including nested updates. |
| #14 Bytecode serialization | Not needed for v1 (see the CGI decision). |

---

## Deferred

| Item | Why, and when |
|---|---|
| `file.read(path)` for raw text in `data/` (overview §4, rendering Markdown from data) | Not needed by M5's samples; small when wanted |
| Session ID rotation and sign-out | With sign-in, M6. `SessionStore` has `removeForUser`, and the user index |
| Session viewing and editing in the dev tools; the request inspector's session before and after | M12's dev tools |
| Configurable timeouts, caps and limits | With `config`, M6; the defaults are in `SessionConfig` and `DataLimits` |
| A `Referer` check when there's no `Origin` | The token is the real check; modern browsers send `Origin` on every POST |
| CSRF exemption for webhooks | With `webhook.x`, M9 |
| Engine log rotation and retention | M11 |
| Cookies set by app code | Not in the overview's API yet; the `ff_` prefix is reserved for the engine |
| The CGI adapter proper: cached compiled app, FastCGI, `cgi-setup` | M10 |

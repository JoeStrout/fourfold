# Fourfold: Milestones — Design Notes

*Status: first sketch, 2026-10-22. Builds on [overview.md](overview.md) (§n) and [architecture.md](architecture.md) (Arch §n). Scope per milestone is a plan, not a promise; items may move as we learn.*

## Approach

- **Milestone 1 is a minimal, working skeleton**: it serves static pages, but through the real component structure, so every later milestone fills in a component rather than restructuring one.
- **After that, risk decides the order.** The milestones that come first are the ones whose outcome could change the design or force large rewrites: the threading model, per-request VM cost, swapping compiled apps, and the template security core. Features that are large but well understood (TLS, job scheduling, the dev panels) come later, when the foundations under them are settled.
- **Each milestone ends with something runnable**, a sample app that exercises what's new, and automated tests. A milestone isn't done until its exit criteria are met, including any measurements.
- **Security features arrive complete or not at all.** A milestone may leave a feature out, but never ships a weakened version of one. Where an early milestone runs without a protection that comes later (e.g. CSRF before M5), it runs only in dev mode on localhost, and says so at startup. (From M5, `fourfold serve` runs on other addresses without sign-in, rate limits, or a per-request memory limit, and says so at startup.)

---

## Risk register

The risks most likely to change the design, and where each is retired:

| # | Risk | Could force | Retired in |
|---|---|---|---|
| R1 | A fresh VM per request is too slow (load the compiled app, build `req`, run the file's top level, call the handler) | Rethinking the §2 execution model, e.g. VM reuse with reset | M2 |
| R2 | Interleaving many VMs on one script thread, with parked VMs for I/O, doesn't work cleanly with MS2's not-done intrinsics, or nests badly (an intrinsic calling back into script) | Redesigning `ScriptHost` and every async intrinsic | M2 |
| R3 | MS2 can't enforce an instruction budget, or remove compiling intrinsics, without deep changes | Changes to MS2; the sandbox claims of §1 | M2 |
| R4 | Per-VM memory budgets aren't feasible with a shared GC | The process model (several processes, restarted on overrun) | M2 (decision), M11 (final) |
| R5 | Compiling (which needs MS2) can't share the script thread without stalling requests; bytecode can't be kept per generation and released safely | Where the compiler runs; bytecode serialization; ownership of compiled apps | M3 |
| R6 | The contextual template parser (Go `html/template`-style) is harder in practice than on paper, or too strict for real templates | Template syntax and rules (§4) | M4 |
| R7 | Session locks and file locks as scheduler waits are awkward, or SQLite-backed state is too slow for every request | Session design (§2), the "all cross-request state in files" rule | M5 |
| R8 | CGI cold starts are too slow to be usable | Whether CGI mode exists; bytecode serialization priority | M5 (spike), M10 (full) |
| R9 | Drain–migrate–swap is hard to make correct (locks across processes, restore on failure) | Migration model (§3) | M8 |
| R10 | Compile-time evaluation of declarations (`access.ms`, `form.x`) can't be cleanly separated from the file's runtime behavior | How declarations are written | M6 |

---

## M1 — Static site skeleton

**Goal:** `fourfold dev ./myapp` serves a folder of static HTML, CSS, and images on localhost, through the real architecture.

**Scope:**
- `base` (`Result`, errors), `partition` (`CodeTree` read-only, path canonicalization, traversal rules).
- `compiledapp`: `CompiledApp` with a `RouteTable` and `StaticIndex`; no scripts or templates yet.
- `compiler`: a first pass that scans `code/pages/` and builds routes for `.html` files (`/about` from `about.html`, `index.html` for folders) and static files; route-collision diagnostics.
- `pipeline` stages Admit, Route, Dispatch (static only), Respond. Lookups only in the route table, never on the filesystem (§5); 404 with `pages/404.html`.
- `http-server`: HTTP/1.1 on localhost, with the chosen event loop and parser; header and body limits.
- `cli`: `fourfold dev`. Creates `private/` and the other partitions if they're missing.
- Content types from an allowlist; `nosniff`; the baseline security headers that don't depend on later features.
- The build: one static library per component, so upward dependencies fail to link (Arch §9). Test harness and a first golden compiler test.

**Decisions made here:** event loop and HTTP parser (made: llhttp, and our own small kqueue/epoll loop in `platform`); build system (made: CMake with presets and Ninja); test framework (made: doctest). See [building.md](../building.md).

**Exit criteria:** a sample static site (nested folders, images, a 404 page) works in a browser; path-normalization tests (`%2e%2e`, `//`, case) pass; golden test of the route table.

**Not yet:** no MiniScript, no templates (HTML is served as is), no recompiling (restart to see changes).

---

## M2 — Scripts in the loop

**Goal:** a page can have a `.ms` file whose `render` and `get`/`post` handlers run in a fresh VM per request, interleaved on the script thread. This milestone is mostly about retiring R1–R4.

**Scope:**
- `script`: `ScriptCompiler` (taking `CodeFile` only), `ScriptHost` with the script thread and scheduler, time slices, wall-clock budget, parked VMs with a worker pool, and results posted back.
- The MS2 work this needs (with the MS2 side): instruction budget; removing or replacing `import` and other compiling intrinsics; an import resolver limited to `code/lib/`.
- Handler discovery: the `render` global and the `get`/`post` maps, recorded in the route table at compile time.
- A frozen `req` (method, path, query, headers, raw form fields as strings), built from request facts.
- Return values: a map from `render`, string (as `text/plain`), `redirect`, `json`, `status`, `null`, and `error`.
- **A temporary, minimal template fill** for `render`'s map, text context only, HTML-escaped, to be replaced by M4. Anything else containing `{{` is a compile error for now, so nothing unsafe can slip through.
- One genuinely asynchronous test intrinsic (e.g. a `sleep` that parks the VM, plus one that does work on the worker pool) to prove the parking path, including an intrinsic that calls back into script (for the later `file.update(path, @f)`).
- Errors: a 500 page, and a plain error page with file and line in dev mode (the full overlay comes in M3).

**Measurements (exit criteria for R1–R4):**
- Requests per second and latency for a trivial `render`, with 1, 100, and 1000 concurrent clients. A target to agree on before starting, e.g. a fresh-VM overhead well under a millisecond.
- An infinite loop, a deep recursion, and a large allocation each end in a friendly error, while other requests keep being served.
- Many VMs parked at once on slow work, with the thread staying responsive.
- **A decision on memory budgets (R4):** per-VM accounting in MS2, or a process-level limit with worker restarts. If the latter, the multi-process model moves earlier, into M5.

**Decisions made here** (details and numbers in [m2-results.md](m2-results.md)): R1 retired. A fresh VM costs about 0.15 ms per run, against a target of 0.5 ms, so the execution model stands, with no VM pooling. R2 and R3 retired, with host-side workarounds for MS2 gaps: handlers are called through precompiled stubs; callbacks use `ManuallyPushCall`; the shared type maps are frozen; budgets are counted per slice. No MS2 changes were made in M2, by decision. **R4 is still open:** one request can allocate gigabytes within its budget. The recommendation is per-VM allocation accounting in MS2, with a process-level limit as backstop, landing before M5 lifts the localhost-only restriction.

**Not yet:** no `data`, `session`, or CSRF. POST handlers run without CSRF protection, so M2 builds refuse to listen on anything but localhost.

---

## M3 — Live recompile and app swap

**Goal:** the edit → save → see loop of §9. Retires R5.

**Scope:**
- `lifecycle`: watching `code/`, waiting for quiet, the `.deploy` trigger, compiling a new version of the app while the old one serves, and swapping. A failed compile keeps the old version.
- App generations: requests pin a generation; `ScriptHost` owns each generation's bytecode and unroots it when the last request using it finishes.
- **Decide where compiling runs** (Arch, Open questions): sliced on the script thread, or in a separate process returning a serialized compiled app. Measure the delay a large compile causes to in-flight requests either way.
- `Observer` interface, and the first of `devtools`: the error overlay (compile and runtime errors, with file, line, and code), and live reload over SSE (`/__dev/reload`), including CSS swapping in place.

**Exit criteria:** editing an HTML, CSS, or `.ms` file shows up in the browser within about two seconds, without a manual reload; a syntax error shows the overlay and the site keeps serving the last good version; a stress test that swaps compiled apps repeatedly under load shows no leaked generations and no GC errors (including with forced full collections between steps).

**Decisions made here** (details and numbers in [m3-results.md](m3-results.md)): R5 retired. Compiling runs on the script thread, sliced a file at a time, with each page's top level run as a scheduled discovery run; no separate process, so bytecode serialization isn't needed for this. Recompiling a 500-page app takes about 1.1 s and delays a request made meanwhile by about 2 ms (5 ms at worst). `code/` is watched by polling through `CodeTree` (250 ms, with 200 ms of quiet), not OS file events; a save reaches the browser in about 0.5 s. 200 swaps under load leave no generations behind. Live reload uses server-sent events, so `HttpServer` gained streamed responses. An app that doesn't compile at startup now starts anyway, and serves an error page until it does.

---

## M4 — Safe templates and Markdown

**Goal:** the full template system of §4, replacing M2's stopgap. Retires R6. This is the core of the XSS story, so its tests are the most thorough in the project.

**Scope:**
- `template`: the HTML tokenizer and context classifier; all the contexts and rules in the escaping tables; boolean attributes and null-omits; dotted paths; `\{{`; page vs. fragment validation; `{{> includes}}` with cycle detection; recording `<script>` and `<form>` positions.
- `FieldSource`, and the script-side implementation over a `Value` map.
- `Html` as a handle; `frag(name, value)` including lists; `unsafeHTML`.
- Missing and extra field warnings, via the `Observer`.
- `markdown`: md4c with no raw HTML, the URL rule, both profiles, heading ids, link rewriting and broken-link warnings, `.md` pages wrapped in `frags/layout/markdown.html` (or the built-in default), `.md` fragments, `markdown()` from code, `markdown.css` injection, the size cap.
- CSP with a fresh nonce per response, added to every `<script>`; HTMX served from a reserved URL, configured without `eval` or inline styles; `hx-on*` as a compile error.
- HTMX-aware responses: `req.htmx`, fragment responses, the HTMX form of `redirect`.

**Exit criteria:** a large table-driven test suite for the escaping rules (each context with hostile inputs, including the known `html/template` bypass cases); a fuzz test of the tokenizer and Markdown renderer for crashes and super-linear time; the guestbook from §7 rendering and swapping fragments with HTMX (with fixed sample entries, since storing them needs `data`, which comes in M5), and a real-world-sized template set from an existing site ported to check the rules aren't too strict.

**Decisions made here** (details and numbers in [m4-results.md](m4-results.md)): R6 is mostly retired. The contextual parser follows the HTML spec's tokenizer and refuses wherever the spec gets subtle (foreign content, script escape states, CSS), which kept it small and linear. In `<script>`, it follows both readings of an ambiguous `/` rather than guessing. The rules weren't too strict for a real site: miniscript.org's 68 HTML files compiled after the expected conversions (pieces of pages moved to `frags/`, inline event handlers replaced by listeners), though with few fields to place, so that part of R6 is only lightly tested. Markdown is md4c's parser (raw HTML off) with a renderer of our own; footnotes are deferred, since md4c has none. Each response gets a fresh CSP nonce, on every `<script>` the engine renders. htmx is served by the engine at `/__fourfold/htmx.js` with eval and script tags off, and pages load it with an explicit tag. A fragment response containing `<script>` is an error. Pages filled by `render` are rendered on the script thread, at no measurable cost. Fuzzing found and fixed one crash in the Markdown renderer.

---

## M5 — State: data and sessions

**Goal:** apps can remember things. Retires R7, and checks R8 with a spike.

**Scope:**
- `Datum`, the canonical codec, and the `Value` ↔ `Datum` bridge, with the single inert check (Arch §4).
- `appdata.DataFiles`: get, set, atomic write, `file.update` with a lock held as a scheduler wait, append, `data.increment`, compare-and-swap; the `data/.fourfold/` reserved folder.
- `state.SessionStore` (SQLite) and `LockService`: lazy load, lazy lock, save only if changed, size cap as a hard failure, timeouts, anonymous sessions created on first write, cookie attributes, hashed IDs, per-IP and total caps on anonymous sessions.
- `flash`.
- **CSRF:** tokens in every `<form method="post">` and in `hx-headers`, the `Origin` check, and rejection before handlers run. With this, the localhost-only restriction from M2 is lifted for non-dev runs.
- The engine log in `private/state/logs/`, and the app's `dropbox` writer.
- **CGI spike (R8):** a minimal CGI adapter running the M5 sample app; measure a cold start (compile, or load a cached compiled app) and a warm one. Decide whether bytecode serialization is needed for v1, and confirm cross-process session locks and counters work.

**Exit criteria:** the guestbook and a visitor counter persist across restarts; a concurrency test (many clients hitting `data.increment` and one session from several tabs) gives exact counts and no lost updates; a session over the cap returns a 500 without saving; the CGI decision is written down.

**Decisions made here** (details and numbers in [m5-results.md](m5-results.md)): R7 retired. Sessions are in SQLite (`private/state/state.db`), with hashed IDs; a run that uses one parks until its lock is granted and it's loaded, and saving it after the run adds no wait. A session adds about 0.25 ms to a request. Whether a route uses the session is decided at compile time (its code names `session`, or its page has `{{flash}}`), since MS2 can't detect a map's first access. Data files are values (`.json`, replaced atomically) or logs (`.jsonl`, appended); `file.update` became `data.update`, built on M2's callback stub. Flash messages are a reserved session key, `session.flash`, removed once a page shows it (changed just after M5 from a `flash` function). The CSRF token comes from a separate cookie, not the session, so visitors who don't write `session` still cost no record. The `Datum` codec is JSON. R8 checked: a CGI cold start is about 5 ms for a small app, but compiling grows with the app, so M10 caches the compiled app's plain parts and compiles only the requested page; **bytecode serialization isn't needed for v1**. Counters and sessions stay exact across concurrent CGI processes. The localhost-only restriction is lifted by a new `fourfold serve`, **without** the per-VM memory limit R4 called for: an accepted risk, stated at startup, until the MS2 change lands.

---

## M5.1 — Files in `data/`

**Goal:** apps can read and write files of their own in `data/`, as text or binary, not only the JSON values and logs of `data`. A small milestone between M5 and M6.

**Why:** M5's `data` module can reach only `name.json` and `name.jsonl`, so an app can't keep a Markdown page, a CSV export, or an image it generated. That's more restrictive than the security model needs. Code still comes only from `code/`, and nothing in `data/` is ever compiled or run, so files there are inert whatever they contain.

**Decisions (2026-10-26):**
- **A `file` module, confined to `data/`** and its subfolders. Paths are relative to `data/`. It goes through `DataDir`, so it has the same rules as `data`: `openat` with no symbolic links followed, no `..`, no dot-names (which also keeps `data/.fourfold/` out of reach).
- **Familiar names.** It follows MiniScript's standard `file` module (command-line MiniScript, Mini Micro, raylib-miniscript) where those names apply: `file.readLines`, `file.writeLines`, `file.loadRaw`, `file.saveRaw`, `file.exists`, `file.info`, `file.children`, `file.makedir`, `file.delete`, and the path helpers `file.name`, `file.parent` and `file.child`.
- **Whole files only, written all at once.** Each write goes through a temporary file, `fsync` and a rename, like `data.set`, so a reader sees the old file or the new one, never part of either. There's no `file.open` or `FileHandle`, no current directory (`curdir`, `setdir`), and no appending (logs are `data.append`'s job). Open handles that outlive an intrinsic call don't fit parked VMs.
- **One set of locks for both modules.** `file` writes and deletes take the same per-file lock as `data`'s writes. `data`'s lock key moves from the data name to the file path, so `file.writeLines("votes.json", …)` can't land in the middle of a `data.update("votes", …)`. **`data` becomes a convenience layer over files:** JSON values and logs, with `update`, `increment` and `compareAndSet`. A file written by `file` that isn't valid JSON is just a damaged value to `data` (an error, as for any damaged file).
- **Text is text; binary is `RawData`.** Strings are for text, not bytes. `file.readLines` and `file.writeLines` work in UTF-8, and reading a file that isn't valid UTF-8 (or holds a NUL) is an error that points to `file.loadRaw`. Binary files use a **`RawData`** class of Fourfold's own, modeled on command-line MiniScript's and keeping its API as far as possible (`len`, `resize`, `littleEndian`, `byte`/`setByte` through `double`/`setDouble`, `utf8`/`setUtf8`), so scripts written for either work alike: `file.loadRaw` returns one, `file.saveRaw` writes one. It's a small amount of code, and owning it avoids an MS2 change: MS2's `RawData` is registered only together with the shell's unrestricted `file` module, and its helpers are private. `RawData` isn't inert (it's a handle), so it can't go into `session`, `data` or `json()`.
- **A size cap per file**, the same for text and binary, defaulting to `data`'s 16 MB, and enforced on `RawData.resize` too, so a buffer can't be grown without limit (R4). It's set in code for now, and becomes a server setting with `config` in M6, alongside `data`'s other limits.
- **Never served by URL.** No URL maps into `data/`, now or later: the route table is built only from `code/pages/`. A handler can read a file and put its contents in a page (escaped, like any value) or render it with `markdown()`. Sending uploads back to visitors stays M7's, through an explicit function called by handler code, with the safety rules of overview §8 (User uploads), never through a path in a URL.

**Scope:**
- `appdata`: the file operations above on `DataFiles` (text, bytes, list, info, folders), sharing its lock service, size cap and worker pool. `data`'s lock key changes to the file path.
- `script/modules/file.cpp`, and `script/modules/raw_data.cpp`: the `RawData` class. Its buffer lives behind a handle (`Value::NewHandle`, freed by the GC when unreachable), checked against the live buffers before use, like `frag`'s and `redirect`'s handles, so a script can't point an instance at anything else. Its class map is frozen, like the other shared type maps; instances aren't, so `littleEndian` can be set.
- Docs: overview §3 (Tier 1) and §4 (`file.read` in the Markdown section becomes `file.readLines`), architecture (`appdata`), and the user guide.

**Exit criteria:** tests that `file` can't leave `data/` (`..`, absolute paths, symbolic links in `data/`, dot-names, `data/.fourfold/`); concurrent `file.writeLines` and `data.update` on one file lose no update; a binary round trip through `RawData`; the size cap on writes and on `RawData.resize`; a Markdown page kept in `data/` and rendered by a handler.

**Decisions made here** (details in [m5.1-results.md](m5.1-results.md)): built as decided above, with no MS2 changes. A path that starts with `/` is an error, not taken as relative to `data/`. Lock keys are file paths, lowercased, so two spellings of one file on a case-insensitive file system share a lock. Besides the 16 MB cap per buffer, all of one request's `RawData` together is capped at 64 MB, since the buffers live outside MS2's heap (R4); reaching it first runs a garbage collection. `RawData` keeps command-line MiniScript's API, including `len =`; `utf8` stops at a zero byte (as there) and shows invalid UTF-8 as U+FFFD. `file.info`'s date is in UTC. `markdown()` takes a list of lines, so `markdown(file.readLines(path), "full")` works. `data.append` stays lock-free: rewriting a log with `file` while appends happen can lose one, which the user guide says.

---

## M6 — Configuration, auth, and access control

**Goal:** sign-in and declared access rules (§5, §10). Retires R10.

**Scope:**
- `DataEvaluator`: the restricted VM for evaluating MiniScript as data (with the MS2 side).
- `config`: `app.ms`, `secrets.ms`, the optional server config, defaults, ceilings, keeping the old config when a new one is invalid; one sub-struct per component.
- Compile-time declarations: `access.ms`, and `access =` in page files, evaluated as data; the effective access level stored on each route; "rule matches nothing" warnings.
- `policy.PolicyGate` and `GateApproval`; denial handling (redirect to sign-in for GET, 401/403 otherwise); `Cache-Control: private, no-store` on restricted responses.
- `state.AuthStore`; `authflow` with magic links, sign-out, sign-out everywhere, and the dev-mode fake login; session ID rotation at sign-in; `req.user`, looked up on every request; the `users` module (grant, revoke).
- `mail`: SMTP and one provider, structured messages only.
- App-styled login pages via `frags/auth/`.
- `devtools`: the access report.

**Exit criteria:** an app with a public area, a members area (including static files), and an admin area, verified by a test that requests every route as each kind of visitor; the access report matches; magic-link sign-in works end to end against a real mail server.

---

## M7 — Forms and abuse protection

**Goal:** declared forms and the spam defenses (§4, Forms and validation; §5, Rate limits and spam).

**Scope:**
- `form.x` declarations read at compile time; all field types and rules; normalization (trim, NFC, control characters); dropping undeclared fields; `req.errors`; `formState` and `reshow`, both with HTMX and without JavaScript.
- Multipart parsing under limits; uploads checked by content; `req.files`; saving uploads to `data/`; serving files from `data/` safely (content-type allowlist, `attachment` by default).
- Checking forms against templates: mismatch warnings, and adding browser-side validation attributes.
- `state.RateStore`; default limits for non-GET requests; `limits.ms`; `ratelimit()` from code; IPv6 /64 grouping; 429 handling.
- Timing tokens (`minSeconds`) and proof-of-work challenges, including the client-side solver.
- `devtools`: limits and form warnings added to the access report; the request inspector shows counted limits and rejections.

**Exit criteria:** the guestbook with a declared form, error redisplay, and spam rules; tampered submissions (extra fields, bad choices, forged file types) are rejected or dropped in tests; a flood test is throttled as configured.

---

## M8 — Databases and migrations

**Goal:** the middle tier and Tier 2 of §3, with migrations and backups. Retires R9.

**Scope:**
- `appdata.Database`: SQLite first, then Postgres; named queries from `code/queries/` as `NamedQuery`; parameter binding; the allowlisted identifier slots; connection pooling; queries run on the worker pool with the VM parked.
- `Migrator`: `.sql` and `.ms` migrations, numbering and checksum rules, records stored with the data (`fourfold_migrations` table, `data/.fourfold/`).
- `lifecycle`: drain–backup–migrate–swap, the schema lock, the waiting queue and its 503 after a limit, refusing to roll back past a migration.
- `BackupService`: `fourfold backup` and `fourfold restore`, automatic backups before migrations, scheduled backups and retention.
- Dev mode: automatic migrations after recompiling, the migrations panel, "redo last", "reset dev data", and `seed.ms`.
- Decide the SQL dialect question (§3, Still open).

**Exit criteria:** a migration that fails partway leaves the database and data files exactly as they were (tested by injecting failures at each step); a long migration under load holds requests and then serves them on the new version; Postgres and SQLite pass the same query tests.

---

## M9 — Outbound HTTP, secrets, webhooks, and jobs

**Goal:** the app can reach out, be called in, and do work outside a request (§6; §2, Background jobs).

**Scope:**
- `outbound.OutboundClient`: `outbound.ms` allowlist, the host config's tightening, private and reserved IP blocking, resolve-check-pin, redirect handling, size and time limits, outbound rate limit, logging with secrets redacted.
- `SecretVault` and `SecretRef`, secret handles in scripts, header and URL parts as lists of strings and handles.
- `webhook.x` handlers with `req.rawBody`, exempt from CSRF.
- The `mail` intrinsic for app code (from the open question in the overview, if decided by then).
- `jobs`: `code/jobs/`, the `job` map, `enqueue` with commit-on-success and unique keys, `schedule.ms`, no self-overlap, missed runs run once, retries with backoff, dead jobs, the job budget and worker cap, jobs on whichever version of the app is current.
- `push`: SSE channels and `publish`, from requests and jobs.
- `devtools`: the outbound report, the jobs panel, outbound calls and enqueued jobs in the request inspector.

**Exit criteria:** SSRF tests (metadata address, localhost, DNS rebinding, redirect to a private address, IPv4-mapped IPv6) are all refused; a secret can't be made to appear in a page, a log, a `data` file, or a request to the wrong host; a weather-API sample works; a job killed mid-run is retried; a feed-polling job publishes to open pages over SSE.

---

## M10 — CGI and FastCGI hosting

**Goal:** the reduced hosting mode of §8, building on the M5 spike.

**Scope:**
- The CGI and FastCGI adapters; caching the compiled app's plain parts in `private/state/`, and compiling per request only the requested page's `.ms` file and its imports (M5 decided bytecode serialization isn't needed); recompiling when `code/` changes or `.deploy` is touched.
- File-based locks for sessions and the schema lock; migrations run by the first request that sees new code.
- `fourfold cgi-setup` (the entry point and `.htaccess`, with the app folder outside the web root); refusing to start if the app folder is inside the web root.
- `fourfold run-jobs` for cron.
- Mode reporting: what CGI mode doesn't provide (SSE, OS-level enforcement), in the dev reports and the engine log.

**Exit criteria:** the M9 sample apps (minus SSE, with polling instead) run on a real shared-hosting account and on a local Apache with suEXEC.

---

## M11 — Production server

**Goal:** Fourfold can face the internet on its own (§8, Serving on the open internet; §1, OS-level enforcement).

**Scope:**
- TLS, with ACME certificates stored and renewed in `private/state/`; port 80 redirects and challenges; HTTP/2.
- Running behind a reverse proxy: trusted proxy addresses, `req.ip`, `Host` checks against `config.domain`.
- All the connection and request limits in the §8 table, including slow-request protection and SSE connection caps.
- The full security header set and CSP configuration (added sources, stricter style option, violation reporting).
- Running without root (socket activation or `CAP_NET_BIND_SERVICE`); a separate OS user per app; file permissions per partition; an append-only dropbox where the OS supports it.
- `front`: the multi-app TLS router over Unix sockets.
- Multiple engine processes per app (`SO_REUSEPORT`), with SSE fan-out between them, if M2's memory-budget decision or load testing calls for it.
- Engine log rotation and retention.

**Exit criteria:** an internet-facing deployment passes an external TLS and header scan; a slowloris and connection-flood test is contained; a load test on a small VPS meets an agreed target; a multi-app server isolates apps by OS user.

---

## M12 — The full product

**Goal:** every feature in the design, ready for a 1.0 release.

**Scope:** everything not yet done, notably:
- The rest of the dev tools (§9): the full request inspector, session viewing and editing, the REPL against the dev instance, and pausing scheduled jobs.
- `fourfold test`: handlers called with synthetic requests and fake users against a throwaway `data/`, using the engine with an in-memory transport.
- The explicit routes table, with parameterized routes and `req.params`.
- The open questions in the overview that are decided for 1.0 (fields in Markdown, non-browser clients, caching and compression, time zones and localization, a production admin console, account deletion and data export, engine and language versioning), each implemented or explicitly deferred past 1.0.
- Redis or Postgres implementations of the engine stores for multi-server deployments, if they're in 1.0's scope.
- Documentation: a tutorial that follows the progressive path (static HTML → a counter → a guestbook → a database app), a reference for every API, and the deployment guides (VPS, container platform, shared hosting).
- A security review of the whole engine, ideally external, against the threat model in §5.

**Exit criteria:** every section of the overview is either implemented or listed as deferred with a reason; the sample apps from every milestone pass as one regression suite; the security review's findings are fixed.

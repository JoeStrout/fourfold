# Fourfold: Milestones — Design Notes

*Status: first sketch, 2026-10-22. Builds on [overview.md](overview.md) (§n) and [architecture.md](architecture.md) (Arch §n). Scope per milestone is a plan, not a promise; items may move as we learn.*

## Approach

- **Milestone 1 is a minimal, working skeleton**: it serves static pages, but through the real component structure, so every later milestone fills in a component rather than restructuring one.
- **After that, risk decides the order.** The milestones that come first are the ones whose outcome could change the design or force large rewrites: the threading model, per-request VM cost, swapping compiled apps, and the template security core. Features that are large but well understood (TLS, job scheduling, the dev panels) come later, when the foundations under them are settled.
- **Each milestone ends with something runnable**, a sample app that exercises what's new, and automated tests. Its docs include what's new for site authors ([user guide](../user/user_guide.md)) and for whoever runs the server ([admin guide](../admin/admin_guide.md), from M6). A milestone isn't done until its exit criteria are met, including any measurements.
- **Security features arrive complete or not at all.** A milestone may leave a feature out, but never ships a weakened version of one. Where an early milestone runs without a protection that comes later (e.g. CSRF before M5), it runs only in dev mode on localhost, and says so at startup. (From M5, `fourfold serve` runs on other addresses without sign-in, rate limits, or a per-request memory limit, and says so at startup; M6 added sign-in and access rules, with sign-in links throttled.)

---

## Risk register

The risks most likely to change the design, and where each is retired:

| # | Risk | Could force | Retired in |
|---|---|---|---|
| R1 | A fresh VM per request is too slow (load the compiled app, build `req`, run the file's top level, call the handler) | Rethinking the §2 execution model, e.g. VM reuse with reset | M2 |
| R2 | Interleaving many VMs on one script thread, with parked VMs for I/O, doesn't work cleanly with MS2's not-done intrinsics, or nests badly (an intrinsic calling back into script) | Redesigning `ScriptHost` and every async intrinsic | M2 |
| R3 | MS2 can't enforce an instruction budget, or remove compiling intrinsics, without deep changes | Changes to MS2; the sandbox claims of §1 | M2 |
| R4 | Per-VM memory budgets aren't feasible with a shared GC | The process model (several processes, restarted on overrun) | M2 (decision); deferred in M11 |
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

**Decisions made here** (details in [m6-results.md](m6-results.md)): R10 retired: `access.ms` and config files run in a restricted VM (`DataEvaluator`, host-side, no MS2 change), and a page's `access =` is read during its compile-time discovery run, which has no data, session or files. The gate now runs for every route, static files included. Mail goes through the system's libcurl (SMTP with required STARTTLS or TLS, and Postmark). The emailed link shows a page, and signing in takes a POST from it, so mail scanners can't use the token. Sign-in links are throttled in `AuthStore` (per address and per visitor), since M7's rate limits come later. Sign-up is open by default (`auth.signUp`). Unknown config settings are errors; an invalid config keeps the previous one, and `fourfold serve` won't start with one. Sign-in links point to `config.domain` in serve mode, never the request's Host. Responses to signed-in visitors are `private, no-store`. The sign-in pages wrap in `frags/auth/layout.html`. Sign-in against a real mail server is left to check by hand.

---

## M6.1 — Markdown as a first-class format

**Goal:** Markdown pages can be fetched as Markdown (`/about.md`), and can have fields, includes and code like HTML pages, so a site can be written entirely in Markdown, dynamic parts included, and read by a Markdown-only client such as a Markdown browser in Mini Micro. A small milestone between M6 and M7.

**Why:** today a `.md` page is only ever sent as HTML, and has no fields and no code, so even a Markdown page with a visitor counter has to become HTML. Markdown's feature set is the right size for MiniScript and Mini Micro, which will never have a full HTML, CSS and JavaScript browser, but could easily have a Markdown one.

**Terminology (2026-11-05):** "FourFold Flavored Markdown" comes in three levels, and the docs and code use these names from here on:
- **Limited Markdown:** the restricted level for visitor-entered text such as comments and posts (headings become paragraphs, images become links, links get `rel="nofollow ugc"`, no `id`s). What M4 called the "comment profile". The default for `markdown()` from code.
- **Full Markdown:** all of the Markdown syntax we support, without our dynamic extensions. `markdown(text, "full")` from code, and what `/x.md` sends to a Markdown client.
- **Dynamic Markdown:** Full Markdown plus fields and includes (`{{name}}`, `{{> intro}}`), so code can change what the visitor sees. Only `.md` files in `code/`; text passed to `markdown()` and Markdown in `data/` is never dynamic.

**Decisions (2026-10-28):** designed in overview §4, [Fields in Markdown](overview.md#fields-in-markdown) and [Markdown as Markdown](overview.md#markdown-as-markdown). In short:
- **`/about.md` serves the page as Markdown**, on by default (replacing the planned opt-in raw-source setting). It's the same route as `/about`, with the same access rules and code, and only Markdown pages have it. Chosen by extension, not by `Accept` header. Links are kept as written, since the source's relative `.md` links already resolve to these URLs. The HTML page links to it with `rel="alternate"`. No layout around the Markdown.
- **Fields in Markdown in `code/`**, with the same syntax as HTML templates (`{{` in prose is a field; `\{{` is literal), never in code spans or code blocks, and never in runtime or `data/` Markdown, which stays inert. `about.md` pairs with `about.ms`.
- **Parse first, fill second; one parse, two outputs.** Each `.md` template compiles to an HTML form and a Markdown form, with each field's position known. Values are escaped for the output: as HTML, or for Markdown by backslash-escaping all ASCII punctuation, turning line breaks into spaces, and protecting leading spaces. The URL rule applies in link destinations in both.
- **Markdown templates take only Markdown.** Includes and rendered values stand alone on a line, at the top level, and are blocks. An HTML fragment or `unsafeHTML` in a Markdown template is an error in both outputs.
- **`frag` and `markdown` handles render late**, in the format of the template they land in. In the Markdown output, `markdown(text)` is written back out as Markdown under its level's rules (Limited or Full).
- **`[[` is reserved in Dynamic Markdown** (decided 2026-11-05) for possible inline form inputs later (see Looking ahead). Wherever `{{` would be a field (prose in `.md` files in `code/`, not code spans or code blocks), `[[` is a compile error saying to write `\[[` for a literal `[[`. Reserving it now means adding form syntax later won't change the meaning of existing pages. Limited and Full Markdown are unaffected: `[[` is ordinary text there.
- **Reading only:** a Markdown client follows links but can't post; Markdown clients that sign in stay with the open question of non-browser clients.

**Scope:**
- `markdown`: parsing with field placeholders and recording each field's context; a Markdown writer beside the HTML renderer (md4c's parser, our renderer), applying Limited Markdown's rules; the Markdown escaping rules.
- **Rename the `"comment"` profile to `"limited"`** everywhere: `markdown(text, profile="limited")`, `MarkdownProfile::Limited`, the wrapper class `markdown limited` (was `markdown comment`), error messages, tests and docs. No alias for the old name; nothing outside this repo uses it yet.
- `template`/`compiler`: `.md` pages and fragments as templates with an HTML form and a Markdown form; `.md` includes (block level, Markdown only, loops an error); `.md` + `.ms` pairing in discovery; the field-placement errors; the reserved-`[[` error; missing- and extra-field warnings; a page with `{{flash}}` uses the session, as for HTML.
- `compiledapp`/`pipeline`: the `/x.md` alias on Markdown routes (route collisions checked, the gate and access unchanged); `text/markdown; charset=utf-8; variant=GFM`; `req.format`; the `rel="alternate"` link in the HTML output.
- `script`: `frag` and `markdown` return late-rendered handles (HTML templates keep their behavior); a format mismatch is a render error.
- Docs: a new **`MARKDOWN.md`** (in `docs/user/`, linked from the user guide and README) that clearly documents FourFold Flavored Markdown on its own: the three levels and where each applies, the syntax each allows (with a feature-by-level table, as in overview §4), how Limited Markdown changes headings, images and links and why, field and include syntax and where fields can't go, escaping with `\{{`, the reserved `[[` and escaping it with `\[[`, the `/x.md` output, and what's not supported (raw HTML, footnotes). It is the reference; the user guide and overview link to it rather than repeating it. Also overview §4 (written with these decisions, terms updated), architecture, the user guide (Markdown pages with code, Markdown as Markdown, `req.format`, the quick reference), and `examples/markdown-site`.

**Exit criteria:** a visitor counter and a guestbook listing (an `.md` fragment per entry) written entirely in Markdown work at `/x` and `/x.md`; a table-driven escaping test for the Markdown output with hostile values (every ASCII punctuation character, line breaks, leading spaces, `|` in table cells, `javascript:` and malformed URLs in destinations, GFM's `www.` and bare-URL autolinks), each checked by parsing the output again and confirming that it has the template's structure, with the value only in text; the same for Limited Markdown written back out (no headings, images or raw HTML get through); a fuzz test of the Markdown writer for crashes and super-linear time; the clubhouse's members-only `.md` page refused at `/x.md` as at `/x`; every existing sample and fixture still compiles, with each Markdown `{{` that was meant as text in a code span or written `\{{`, and likewise each `[[` (in a code span or written `\[[`); `[[` in a Dynamic Markdown page's prose is a compile error, while in a code span, in a code block, or in text passed to `markdown()` it is plain text.

**Decisions made here** (details in [m6.1-results.md](m6.1-results.md)): fields become placeholders of Unicode noncharacters while md4c parses, and the renderer reports where each landed; the Markdown form is the source split at its fields, and the Markdown writer is only for `markdown()` values. `frag` of an `.md` fragment renders its Markdown form only when the output is Markdown, and `markdown()` writes its Markdown only if asked (a handle can't keep MiniScript values to render later). More places a field can't go: inside `[brackets]` that aren't a link, in a bare web or email address, after `<`, inside raw HTML, and in a refused URL. A block (an include or a rendered value) must have a blank line after it as well as before. Limited Markdown's image inside a link is now its text. The access report lists each `/x.md` URL. Known limits: an empty value or emphasis tight against a field mid-word can leave the template's markers showing as text in the Markdown output.

**Looking ahead (not in M6.1):** Dynamic Markdown may later declare form inputs inline, with syntax like `[[age:type=int,width=5]]`, so a Markdown page (and a Markdown client) could post as well as read. That would build on M7's form declarations. M6.1 reserves `[[` (above) so this can be added without breaking existing pages; `MARKDOWN.md` should mention it as a possible future extension, and the Markdown parsing work should avoid choices that would make a second kind of placeholder hard to add.

---

## M7 — Forms and abuse protection

**Goal:** declared forms and the spam defenses (§4, Forms and validation; §5, Rate limits and spam).

**Scope:**
- `form.x` declarations read at compile time; all field types and rules; normalization (trim, NFC, control characters); dropping undeclared fields; `req.errors`; `formState` and `reshow`, both with HTMX and without JavaScript.
- Multipart parsing under limits; uploads checked by content; `req.files`; saving uploads to `data/`; serving files from `data/` safely (content-type allowlist, `attachment` by default).
- Checking forms against templates: mismatch warnings, and adding browser-side validation attributes.
- `state.RateStore`; default limits for non-GET requests; `limits.ms`; `ratelimit()` from code; IPv6 /64 grouping; 429 handling.
- The visitor's real address behind a reverse proxy: a server setting naming trusted proxy addresses, whose `X-Forwarded-For` is used only for requests from them (never trusted otherwise). Until then, everyone behind a proxy shares one address group, so M6's per-address sign-in throttle and M5's anonymous-session caps count the whole site together (admin guide, "Behind a reverse proxy").
- Timing tokens (`minSeconds`) and proof-of-work challenges, including the client-side solver.
- `devtools`: limits and form warnings added to the access report; the request inspector shows counted limits and rejections.

**Exit criteria:** the guestbook with a declared form, error redisplay, and spam rules; tampered submissions (extra fields, bad choices, forged file types) are rejected or dropped in tests; a flood test is throttled as configured, including behind a trusted proxy (visitors told apart by forwarded address; a forged `X-Forwarded-For` from an untrusted peer ignored).

**Decisions made here** (details in [m7-results.md](m7-results.md)): `form.x` is for `post.x` only. With a declaration, every declared field is in `req.form` (empty text `""`, a bad or empty number, date or choice `null`), and `formState` gives a `choice` or `choices` as a map from option to 1 or 0, so templates re-check radio buttons with `checked="{{mood.happy}}"`. `reshow` without JavaScript keeps its state in the session's engine part and redirects to the page beside the handler's file. NFC comes from utf8proc, vendored. Uploads are checked by magic numbers, for the types Fourfold can recognize only; `req.files.x.data` is `RawData`, saved with `file.saveRaw`; `file.send` serves from an allowlist of types, downloading anything that could hold script. Rate limits are sliding windows in `state.db` (no memory cache yet); only requests that pass access and CSRF are counted, and refused ones count nothing. Default limits (60/minute anonymous per address, 300/minute per user) are in `config.limits`; a route's own rules count every method. Timing tokens go in every post form, bound to the CSRF cookie, not single-use. Puzzles are SHA-256, bound to the CSRF cookie, good once for 24 hours, only in forms the compiler matched to a challenge route; 16 bits, 4 in dev. Trusted proxies are `config.proxy.trust`, with `X-Forwarded-For` read from the right. There's no request inspector yet; `/__dev/limits` lists recent counts and refusals.

---

## M8 — Databases and migrations

**Goal:** the middle tier and Tier 2 of §3, with migrations and backups. Retires R9.

**Scope:**
- `appdata.Database`: SQLite first, then Postgres; named queries from `code/queries/` as `NamedQuery`; parameter binding; the allowlisted identifier slots; connection pooling; queries run on the worker pool with the VM parked.
- `Migrator`: `.sql` and `.ms` migrations, numbering and checksum rules, records stored with the data (`fourfold_migrations` table, `data/.fourfold/`). Folder migrations (§3, decided 2026-11-05): `migrate.ms` with its own `.sql` queries, which are the only ones its `db` can reach; no imports from outside the folder; a single-file `.ms` migration that names `db` is a compile error; the checksum covers the folder's files for the active dialect; all of a migration's database work in one transaction.
- `lifecycle`: drain–backup–migrate–swap, the schema lock, the waiting queue and its 503 after a limit, refusing to roll back past a migration. In production, only on an explicit `fourfold migrate` (decided 2026-11-05): a new version with pending migrations isn't swapped in until then, `fourfold migrate` asks a running server (or applies them itself if none is running), and `fourfold serve` won't start with migrations pending unless given `--migrate`.
- `BackupService`: `fourfold backup` and `fourfold restore`, automatic backups before migrations, scheduled backups and retention.
- Dev mode: automatic migrations after recompiling, the migrations panel, "redo last", "reset dev data", and `seed.ms`.
- SQL dialects (§3, decided 2026-11-05): `{{name}}` and `{{0}}`-style index placeholders (the only placeholder syntax), with values in a map, or in a list when every key is an index (an index finds a number key or its string in a map; missing keys an error, extra keys or items ignored), rewritten for each database by a scanner that understands each dialect's strings and comments; `{{` inside a string literal or quoted identifier a compile error; `.sqlite.sql` and `.pg.sql` variants for queries and migrations, with the compile-time rules (including that all variants use the same set of keys) and checksums of the applied file; the "portable SQL" guide in the user docs.

**Exit criteria:** a migration that fails partway leaves the database and data files exactly as they were (tested by injecting failures at each step); a long migration under load holds requests and then serves them on the new version; Postgres and SQLite pass the same query tests.

**Decisions made here** (details in [m8-results.md](m8-results.md)): one placeholder syntax, `{{key}}`, a name or an index, with values from a map or (all indexes) a list; `.sqlite.sql` and `.pg.sql` variants with the same keys; name slots with their allowlist inline, `{{sort: title, created}}`, inserting the chosen name as written. A `.ms` migration that uses the database is a folder with its own queries and helpers, importing nothing else. A batch of migrations does all its database work in one transaction, so a failure undoes the batch; data files come back from the backup taken first. In production, migrations wait for `fourfold migrate` (which asks a running server through a request file in `private/state/control/`); `serve` won't start with any pending, unless `--migrate`. SQLite is `data/.fourfold/app.sqlite`, opened on first use; Postgres is `config.db`, with its password from an engine-only secret. Backups are folders in `private/state/backups/` (or `config.backups.folder`); "redo last" and "reset dev data" are SQLite only. One Fourfold at a time runs an app (a lock in `private/state/locks/`).

---

## M9 — Outbound HTTP, secrets, webhooks, and app mail

**Goal:** the app can reach out and be called in (§6), and send its own mail. The first half of what was planned as one M9; jobs and push follow in M9.1.

**Decisions (2026-11-12, with the user):**
- **Split M9.** The security core (outbound HTTP, secret handles, webhooks, app mail) is a milestone of its own, so it can be reviewed before jobs and push build on it. Jobs, the schedule and SSE are M9.1.
- **App mail goes to users and an allowlist**, closing the overview's open question. `mail.send(to, subject, template, fields)`: `to` is a user's `id` (the engine looks up the address, so the app never needs it) or an address the app allows in `code/outbound.ms` (`mailTo = ["owner@example.com", "*@example.com"]`), for things like a contact form sent to the site owner. `mailTo = ["*"]` allows any address, flagged in the outbound report like `allow = ["*"]`. Structured fields only, never raw headers; the subject has its control characters refused; the body is a Markdown template in `code/mail/` (`welcome.md`), sent as text (its Markdown form) and HTML. A per-app send limit (`config.mail.appPerHour`, default 100); `config.mail.appMail = false` turns it off. Sent through the transport in `config.mail`, on the worker pool with the VM parked.

**Scope:**
- `outbound.OutboundClient` (libcurl, already used for Postmark): `code/outbound.ms` read at compile time (`allow`: hostnames, `*.` for subdomains, `"host:port"`, `"*"`; HTTPS only unless written `"http://host"`); `config.outbound`, which can only tighten (`enabled`, `allow` as a cap, `allowAddresses` for named internal addresses, `perMinute`, `maxResponseKB`, `timeoutSeconds`). Always on: private and reserved addresses refused (loopback, RFC 1918, link-local, CGNAT, ULA, `0.0.0.0/8`, multicast, broadcast, documentation ranges; IPv4-mapped and -compatible IPv6, NAT64 and 6to4 judged by the IPv4 address inside); every address checked as it's connected to (Host and SNI kept); redirects followed by the engine, each hop checked again, at most 5, never HTTPS to HTTP; response size limited after decompression; connect and total timeouts within the request's budget; a per-app outbound rate.
- `http` module: `http.get(url, headers)`, `http.post(url, body, headers)`, `http.put`, `http.patch`, `http.delete`, and `http.request(method, url, body, headers)`. A body map or list is sent as JSON. The response map: `status`, `headers` (lowercased names), `body` (text) or `raw` (a `RawData`, for bytes that aren't UTF-8 text), and `json` (inert values, or null). Failures are error values. Runs on the worker pool with the VM parked.
- `SecretVault` and `SecretRef`: `secrets.name` is a handle. A URL or header value is a string, a handle, or a list of strings and handles, joined only as the request goes out, and only to the secret's `hosts` (checked on every redirect hop; a hop to another host drops the request). A handle prints as its type, and `data`, `session`, `json`, template fields and `enqueue` refuse it. An engine-only secret (no `hosts`) is absent, with a dev-mode error saying why.
- `webhook.x`: `POST /<page>/x` with no CSRF token; `req.rawBody` only there; access rules and rate limits apply. `hmac.verify(algorithm, secret, message, signature)` checks a provider's signature (constant time; SHA-256, hex or base64), with a key that's a handle: a secret with `"verify": true` in `secrets.ms` (it's never sent anywhere) or one with `hosts`. There's no way to compute an HMAC that script can see.
- `mail` module, as decided above.
- Logging: each outbound call and each app mail in the engine log (host or recipient, status, time, bytes), with secrets and query strings redacted.
- `devtools`: the outbound report at `/__dev/outbound` (allowed hosts and `"*"`, `mailTo`, which secrets go to which hosts, webhook endpoints, and the recent outbound calls with refusals).

**Exit criteria:** SSRF tests (the metadata address, localhost by name and by number, every blocked range including IPv4-mapped IPv6, a name resolving to both a public and a private address (never connected to at the private one), DNS rebinding, a redirect to a private address and to a host off the list, HTTPS to HTTP) are all refused; a secret can't be made to appear in a page, a log, a `data` file, the session, a mail, or a request to the wrong host (including through a redirect); size, time, decompression-bomb and rate limits hold; a webhook with a good signature is accepted, and one with a bad or missing one refused, without a CSRF token; app mail goes to a user by id and to an allowed address, and is refused to others; a weather-API sample works (tested against a local server allowed by `allowAddresses`, and by hand against the real API).

**Decisions made here** (details in [m9-results.md](m9-results.md)): addresses are checked in libcurl's open-socket hook as each connection is made, not resolved and pinned first, so a name with public and private addresses is connected to only at the public ones; NAT64 and 6to4 addresses are judged by the IPv4 address inside rather than refused, so IPv6-only hosts still work. `http` and `mail` run on a separate network pool (8 threads), so a slow API can't hold up session and database work; a call's deadline is fixed when it's made. A response of any status is a map; only a refused or failed request is an error value; `raw` holds a body that isn't text. Header names go out as the app wrote them. A redirect to another host drops `Authorization` and `Cookie` (without secrets) and fails if a header carries a secret. `hmac.verify` is SHA-256 only; a sending secret may verify too, an engine-only one never. Webhooks also get `req.json`, and skip the timing check and puzzle (a `limits.ms` rule asking for them warns); `post.x` beside `webhook.x` is a compile error. Mail templates are `code/mail/*.md`; `mail.send` returns null, and counts each recipient against `appPerHour` in the `RateStore`. Blind copies (added with the user after the first build): `options.bcc`, up to 10, by the same rules as `to`, in no header; no CC. The outbound report is `/__dev/outbound`; there's still no request inspector.

---

## M9.1 — Jobs and push

**Goal:** the app can do work outside a request, and push updates to open pages (§2, Background jobs; §7).

**Decisions (2026-11-12, with the user):**
- **SSE channels are paths under the access rules.** A page subscribes to `/live/<name>`; `access.ms` rules apply to that path like any page's (public, signed in, a role), and a channel no rule or route covers is refused as an unmatched route would be. `publish "orders", html` sends to subscribers of `/live/orders` only. Per-user channels can come later.
- **Schedules use IANA time zones** (`config.jobs.timeZone = "America/Denver"`, default UTC), read from the system's zoneinfo files. A time skipped by a DST change runs at the jump; a time that happens twice runs once.

**Scope:**
- `jobs`: `code/jobs/`, the `job` map, `enqueue` with commit-on-success (in jobs too: a job's enqueues commit only if it succeeds) and unique keys, `code/schedule.ms` (friendly forms and cron), no self-overlap, missed runs run once, retries with backoff, dead jobs, the job budget and worker cap, jobs on whichever version of the app is current; `JobQueue` in `private/state/`.
- `push`: `PushHub`, SSE channels and `publish`, from requests and jobs; HTMX's SSE extension served by the engine.
- `devtools`: the jobs panel (queue, recent runs, dead jobs with retry and discard, the schedule with next run times, run now, pause), and channels in the access report.

**Exit criteria:** a job killed mid-run is retried; enqueues from a failed request are dropped; a unique key enqueues once; schedules fire on time across a DST change in a named zone; missed runs run once after downtime; a feed-polling job publishes to open pages over SSE; a channel under a members-only rule refuses a visitor who isn't signed in.

**Decisions made here** (details in [m9.1-results.md](m9.1-results.md)): a job's enqueues and publishes commit only if it succeeds, like a request's (closing the overview's open question), and a request's publishes are held back the same way. `"public"` is an access value: it changes no page (rules only tighten) but declares a channel, since a channel needs a covering rule. A key is unique while its job is queued, not while it runs. A scheduled tick is a queued job keyed `schedule:<name>`, which is how it never overlaps itself. `"every"` schedules are intervals since the last run, first run when the schedule appears. Time zones come from the system's zoneinfo, with the POSIX footer rule for later times. The pause toggle pauses the schedule, not the queue. HTMX's SSE extension is part of `/__fourfold/htmx.js`. A published value can't carry the publishing visitor's CSRF token. `.md` fragments now carry their Markdown form even on HTML output when it renders cleanly, so a job can put one in a mail template. Retries: 15 s, doubling, at most an hour. `fourfold run-jobs` (CGI mode) is left for M10.

---

## M10 — CGI and FastCGI hosting

**Goal:** the reduced hosting mode of §8, building on the M5 spike.

**Decisions (2026-11-12, with the user):**
- **FastCGI as shared hosts launch it.** The process accepts FastCGI on a listening socket inherited as fd 0 (how `mod_fcgid` starts it; a shared host won't let us run a daemon of our own), and also takes `--listen <socket>` so a local `mod_proxy_fcgi` can test the same code.
- **A warm FastCGI process keeps the compiled app in memory** and checks for changes on each request, as CGI does. It doesn't run jobs (`mod_fcgid` kills idle processes); `fourfold run-jobs` from cron is the job runner in both modes.
- **The entry point is a two-line shell script** in the web root that `exec`s the real binary, kept outside the web root, with the app folder's path, since suEXEC runs only programs inside the docroot that the user owns and others can't write.
- **The Linux binary for shared hosts is fully static (musl)**, built in a container, so one binary runs on whatever glibc the host has. If a dependency won't build statically, we fall back to building on an old-glibc image. (The test host, DreamHost shared hosting, is x86_64 Ubuntu 24.04 with glibc 2.39, so an ordinary Linux build also runs there.)
- **Testing, in four layers:** automated tests (CGI environment parsing, the FastCGI protocol, `fourfold cgi` as a child process); the Apache that ships with macOS, run unprivileged from a generated `httpd.conf` on a high port (CGI and `mod_proxy_fcgi`; skipped where `httpd` is missing); Apache with suEXEC and `mod_fcgid` in a Debian container; and the user's DreamHost account, over SSH.

**Scope:**
- The CGI and FastCGI adapters; caching the compiled app's plain parts in `private/state/`, and compiling per request only the requested page's `.ms` file and its imports (M5 decided bytecode serialization isn't needed); recompiling when `code/` changes or `.deploy` is touched.
- File-based locks for sessions and the schema lock; migrations run by the first request that sees new code.
- `fourfold cgi-setup` (the entry point and `.htaccess`, with the app folder outside the web root); refusing to start if the app folder is inside the web root.
- `fourfold run-jobs` for cron.
- Mode reporting: what CGI mode doesn't provide (SSE, OS-level enforcement), in the dev reports and the engine log.

**Exit criteria:** the M9 sample apps (minus SSE, with polling instead) run on a real shared-hosting account and on a local Apache with suEXEC.

**Decisions made here** (details in [m10-results.md](m10-results.md)): a version is cached as a snapshot of `code/` plus every `.ms` file's handlers (running each file's top level was most of a compile), and a request compiles the rest again from the snapshot, MiniScript only for its own page and imports; no `CompiledApp` serialization. Requests serve from the snapshot, so a partial or broken upload never shows. Quiet is judged across requests (a change found unchanged two seconds after it was first seen); `.deploy` skips the wait. A new version's migrations are applied by the request that installs it, under a cross-process schema lock (two lock files, so migrating can't be starved), with a backup first. `publish` succeeds and goes nowhere in CGI mode. `run-jobs` holds the schema lock a job at a time and stops when nothing is due. `cgi-setup` sends everything, static files included, to Fourfold, with `CGIPassAuth On` for webhooks (suEXEC drops rewritten variables). Tested under macOS's Apache (CGI, `mod_proxy_fcgi`) and Debian's with suEXEC (CGI, `mod_fcgid`), with a static musl build that passes the whole suite on Linux.

---

## M11 — Production server

**Goal:** one app can face the internet on its own (§8, Serving on the open internet).

**Decisions (2026-11-13, with the user):**
- **TLS is OpenSSL 3; HTTP/2 is nghttp2.** Both are already in the static musl build (through libcurl) and on every Linux. The ACME client is our own: HTTP-01 challenges, ECDSA account keys and JWS, and certificates through OpenSSL.
- **R4 is deferred again.** MS2 still has no per-VM memory accounting (ms-source-changes.md #1). The engine stays one process per app, with the service manager's memory limit (`MemoryMax=` in systemd) as the guard, and the docs and startup notice still say so.
- **Split in two,** as M9 and M9.1 were. M11 is a single app on the open internet; M11.1 is several apps on one server, isolated by OS user.
- **Testing:** the user provides a small Linux VPS (SSH) and a domain pointed at it for the exit criteria. Until then, ACME is tested against Pebble (Let's Encrypt's test server) in Docker, and everything else locally.

**Scope:**
- TLS, with ACME certificates stored and renewed in `private/state/`; port 80 only redirects and answers challenges; HTTP/2 over TLS (ALPN).
- Running behind a reverse proxy: `Forwarded` as well as `X-Forwarded-For`, `req.ip`, `Host` checks against `config.domain`.
- All the connection and request limits in the §8 table, including slow-request protection (header time, minimum body rate), per-IP connection caps, concurrent-request admission with 503, and SSE connection caps.
- The full security header set and CSP configuration (added sources, the stricter style option, allowed embedding sites, HSTS options, enabled browser features), and violation reporting (the dev overlay; the engine log in production, rate-limited).
- Running without root: systemd socket activation, or `CAP_NET_BIND_SERVICE`; a sample systemd unit.
- Graceful shutdown (stop accepting, finish requests in flight); engine log rotation and retention.

**Exit criteria:** an internet-facing deployment passes an external TLS and header scan; a slowloris and connection-flood test is contained; a load test on a small VPS meets an agreed target.

**Outcome (2026-11-13):** on a test VPS (`test.miniscript.org`), SSL Labs and Mozilla Observatory both gave A+, and slowloris and a one-address connection flood from outside were contained. **Deferred, with the user:** the load test (no target agreed; it belongs with M11.1's multi-process question) and watching a real certificate renew over days (tested with Pebble's 6-day certificates; seeing it with Let's Encrypt needs ACME profile support or two months).

**Decisions made here** (details in [m11-results.md](m11-results.md)): TLS over memory BIOs, so connections do their own socket I/O and HTTP/2 sits on the same plaintext; no session resumption. The header timeout starts with the connection (covering the handshake); bodies are held to an average rate after a grace period. Admission (requests at once, then a short queue, then 503) wraps the handler at the transport. Stream caps are per visitor address; loopback is exempt from per-address caps. Certificates renew with 30 days or a third of their life left, whichever is sooner. Port 80 redirects only to the app's own names. `Forwarded` is read ahead of `X-Forwarded-For`. CSP sources added in `config.headers` are validated (no keywords, no `*`).

---

## M11.1 — Several apps on one server

**Goal:** a server can host several apps, each isolated from the others by OS user (§1, OS-level enforcement; §8, several apps on one server).

**Scope:**
- `front`: the multi-app TLS router over Unix sockets, configured by the server config.
- A separate OS user per app; file permissions per partition; an append-only dropbox where the OS supports it.
- Multiple engine processes per app (`SO_REUSEPORT`), with SSE fan-out between them, if load testing calls for it (R4 stays deferred; see M11).

**Exit criteria:** a multi-app server isolates apps by OS user.

---

## M12 — The full product

**Goal:** every feature in the design, ready for a 1.0 release.

**Scope:** everything not yet done, notably:
- The rest of the dev tools (§9): the full request inspector, session viewing and editing, the REPL against the dev instance, and pausing scheduled jobs.
- `fourfold test`: handlers called with synthetic requests and fake users against a throwaway `data/`, using the engine with an in-memory transport.
- The explicit routes table, with parameterized routes and `req.params`.
- The open questions in the overview that are decided for 1.0 (non-browser clients, caching and compression, time zones and localization, a production admin console, account deletion and data export, engine and language versioning), each implemented or explicitly deferred past 1.0.
- Redis or Postgres implementations of the engine stores for multi-server deployments, if they're in 1.0's scope.
- Documentation: a tutorial that follows the progressive path (static HTML → a counter → a guestbook → a database app), a reference for every API, and the deployment guides (VPS, container platform, shared hosting).
- A security review of the whole engine, ideally external, against the threat model in §5.

**Exit criteria:** every section of the overview is either implemented or listed as deferred with a reason; the sample apps from every milestone pass as one regression suite; the security review's findings are fixed.

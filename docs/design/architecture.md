# Fourfold: Code Architecture — Design Notes

*Status: first sketch, 2026-10-22. Builds on [overview.md](overview.md); section references (§n) point there. Component and type names are illustrative, not final.*

## Purpose

Fourfold is a large program: a web server, a compiler, a template engine, a script host, several stores, an HTTP client, a job scheduler, and a set of dev tools. This document divides it into **components** that can each be understood, built, tested, and changed while ignoring the internals of the others.

The approach:

1. Identify the few **seams** where components meet, and make each one a small, explicit data type or interface.
2. Fix a **dependency direction**, so that changes ripple only one way.
3. Turn the security rules in the overview into **types**, so a component can't break a rule it doesn't know about.
4. Settle the **threading model** early, since MiniScript 2's runtime constrains it, and it affects every component that touches a VM.

---

## 1. Constraints from MiniScript 2

Several facts about the MS2 C++ core (`~/svnrepo/miniscript2`) shape the architecture.

| Fact | Consequence |
|---|---|
| The GC (`GCManager`) is **process-global static state**, with no locking. Every `Value` that refers to a list, map, big string, function, error, or handle points into it. | **Only one thread may touch `Value`s.** All VMs in a process run on that one thread. (See §3.) |
| A `ValueDict`/`ValueList` held by host code is **not a GC root** unless wrapped and rooted (`StaticMap`, `AddRoot`). | Host code should hold `Value`s as briefly as possible. Long-lived host state should be in ordinary C++ types, converted to `Value`s on demand. |
| An intrinsic can return a **not-done `IntrinsicResult`**, and the VM resumes it later; `vm.yielding` lets the host regain control. `RunUntilDone(timeLimit, returnEarly)` runs a time slice. | Many request VMs can be **interleaved on one thread**. An intrinsic that waits on I/O parks its VM instead of blocking the thread. |
| `Interpreter::Reset(List<FuncDef>)` loads already-compiled functions. | A compiled image can be compiled once and loaded into each fresh VM cheaply (§2 of the overview). |
| **Handles** (`Value::NewHandle(ptr, finalizer)`) are opaque and unforgeable by scripts. | Rendered HTML, secrets, uploads, and DB handles are all handles (§4, §6). |
| **Frozen** maps and lists exist. | `req`, `req.user`, and `args` are frozen. |
| `String` (in `CS_String.h`) is `shared_ptr`-based and immutable, *not* GC-managed. `List<T>` and `Dictionary<K,V>` (`CS_List.h`, `CS_Dictionary.h`) are `shared_ptr`-based **reference types** with C# semantics. | These are safe off the script thread, but their reference semantics are surprising in C++ (see §4). |

**Things Fourfold needs that MS2 may not have yet** (to confirm with the MS2 side):

- An **instruction budget** per VM (count, then error), separate from wall-clock slices.
- A **memory budget** per VM. Hard with a shared GC; possible approaches are per-VM allocation counters, or a process-level limit as a backstop (§3).
- A way to **remove or replace compiling intrinsics** (`import`, any eval-like path) per VM, and an **import resolver hook** so `import` resolves within `code/lib/` through Fourfold.
- **Bytecode serialization**, so CGI mode can load a cached image rather than recompiling (§8 of the overview). Without it, CGI mode recompiles `.ms` files when its cache is cold.
- A clean **restricted VM** configuration (no I/O intrinsics) for evaluating declaration and config files as data.

---

## 2. Components at a glance

```
                ┌───────────────────────────────────────────────────────────┐
  Hosts         │  cli   http-server   cgi/fastcgi   front    devtools      │
                └──────────────┬───────────────────────────────┬────────────┘
                     HttpRequest/HttpResponse            Observer events
                ┌──────────────▼───────────────────────────────▼────────────┐
  Engine        │  pipeline   policy   authflow   jobs   push   lifecycle   │
                └───┬──────────────┬──────────────────┬──────────────┬──────┘
                 AppImage       RunContext        store interfaces   AppConfig
                ┌───▼─────┐  ┌─────▼──────┐  ┌────────▼────────┐ ┌───▼─────┐
  Subsystems    │compiler │  │ script     │  │ state  appdata  │ │ config  │
                │         │  │ (VMs +     │  │ outbound  mail  │ │         │
                │         │  │  bindings) │  │                 │ │         │
                └───┬─────┘  └─────┬──────┘  └────────┬────────┘ └───┬─────┘
                ┌───▼──────────────▼──────────────────▼──────────────▼──────┐
  Libraries     │  template   markdown   image (the AppImage model)         │
                └──────────────────────────────┬────────────────────────────┘
                ┌──────────────────────────────▼────────────────────────────┐
  Foundation    │  base (Datum, codec, crypto, errors, time)  partition     │
                │  sqlite   net   threads/event loop   log                  │
                └───────────────────────────────────────────────────────────┘
```

**Dependencies point down only.** A component may use anything below it, and nothing above it. Components in the same row don't depend on each other except where noted. In particular:

- Nothing below the engine knows about HTTP requests.
- Nothing except `script` (and its callers' narrow use of `ScriptHost`) knows about MiniScript `Value`s. The compiler uses MS2 to compile, and `config` uses it to evaluate data files, but both do so through `script`'s small APIs.
- Nothing knows about dev mode except `devtools`, which listens to events and adds routes. The core never calls into it.

| Component | One-line job | Main seam it offers |
|---|---|---|
| **base** | Inert data (`Datum`), serialization, crypto, error/result types, clock | `Datum`, `Result<T>` |
| **partition** | The four file areas, with access rules as types | `CodeTree`, `DataDir`, `Dropbox`, `ConfigDir`, `StateDir` |
| **sqlite**, **net**, **threads**, **log** | Thin wrappers over third-party libraries and the OS | |
| **template** | Parse templates with contextual escaping; render them | `CompiledTemplate`, `FieldSource` |
| **markdown** | Fourfold Markdown (md4c, two profiles) | `renderMarkdown(text, profile)` |
| **image** | The data model of a compiled app | `AppImage` |
| **compiler** | `CodeTree` → `AppImage` + diagnostics | `compile(CodeTree, …)` |
| **config** | `private/config` + server config → typed, clamped settings | `AppConfig` |
| **script** | The script thread, VMs, scheduling, budgets, intrinsic modules | `ScriptHost`, `RunContext` |
| **state** | Engine-owned stores in `private/state/` | `SessionStore`, `AuthStore`, `RateStore`, `JobQueue`, … |
| **appdata** | The app's data: Tier 1 files, SQLite/Postgres, migrations | `DataFiles`, `Database`, `Migrator` |
| **outbound** | Outbound HTTP with allowlist, IP rules, secret substitution | `OutboundClient`, `SecretVault` |
| **mail** | Sending email (SMTP or a provider) | `Mailer` |
| **pipeline** | Turns an `HttpRequest` into an `HttpResponse` | `Engine::handle` |
| **policy** | Access, CSRF, rate limits, timing and challenge checks | `PolicyGate` |
| **authflow** | The `/__auth/*` pages: magic links, fake login, sign-out | routes |
| **jobs** | Schedule evaluation and the job worker loop | |
| **push** | SSE channels and subscribers | `PushHub` |
| **lifecycle** | Watch, compile, drain, migrate, swap; backups | `AppInstance` |
| **devtools** | Overlay, inspector, reports, panels, live reload | `Observer` implementation |
| **hosts** | `fourfold` CLI, standalone server, CGI/FastCGI adapters, front process | `main()` |

---

## 3. Threads and processes

### The script thread

Because the MS2 GC is global and unsynchronized, each engine process has exactly **one script thread**, which owns every `Value`. All request, job, and migration VMs run on it, interleaved by a small scheduler inside `ScriptHost`:

- Runnable VMs get time slices (`RunUntilDone(slice, true)`).
- An intrinsic that must wait (outbound HTTP, a database query, a lock held by someone else, SMTP) **submits work to another thread and parks its VM** with a not-done result. When the work completes, the result is posted back to the script thread, and the VM becomes runnable again.
- Wall-clock budgets are checked by the scheduler; instruction budgets by the VM (once MS2 supports it).

The rule that follows is simple and strict: **`Value`s never leave the script thread.** Everything crossing a thread boundary is ordinary C++ data: `Datum`, byte strings, or plain structs. This rule is what keeps `script` separable from everything else, and it also keeps secret values out of the script thread entirely (§5, outbound).

**Waits must be scheduler waits, not blocking calls.** Two VMs on the same thread can contend for one session lock. If the second blocked the thread on a mutex, the first could never run to release it. So the session lock, file locks for `file.update`, the schema lock, and the job-queue claim are all implemented as "park until granted", never as a blocking acquire on the script thread.

Quick local work (reading a small template file, serializing a session) may run directly on the script thread. Anything that can wait on the network, on another process, or on a lock goes to the worker pool.

### Other threads

| Thread(s) | Does |
|---|---|
| **I/O loop** (one, or a few) | Accepts connections, parses HTTP under limits, TLS, writes responses, holds SSE connections |
| **Script thread** | Runs all VMs; renders templates (field values are `Value`s) |
| **Worker pool** | SQLite and Postgres queries, file locking and atomic writes, outbound HTTP, DNS, SMTP, backups |
| **Compile thread** | Builds a new `AppImage` while the old one keeps serving. The compiler needs MS2 to compile `.ms` files, so this step is posted to the script thread in pieces, or the compiler uses a separate process (see Open questions) |

### Using more than one core

One script thread per process means one core for script execution per process. Two ways to scale, both hidden behind `ScriptHost`:

1. **Several engine processes per app** sharing the listening socket (`SO_REUSEPORT`). This already works by design: the overview requires that **all state crossing requests live in SQLite or files**, for CGI mode's sake (§8, Hosting modes). The one exception is SSE `publish`, which would need fan-out between processes (e.g. a Unix-socket broadcast among siblings).
2. **Making the MS2 GC per-thread** (`thread_local` sets), giving several script threads in one process. Cleaner, but a change to MS2.

v1 can run one process with one script thread, and keep both options open by never letting other code assume a single script thread.

---

## 4. Data types: STL, MiniScript, and `Datum`

The guidance, in short:

| Kind of data | Type |
|---|---|
| Engine code in general: buffers, headers, paths, config, protocol data | **STL**: `std::string` (UTF-8 bytes), `std::string_view`, `std::vector`, `std::unordered_map`, `std::optional`, `std::variant`, `std::shared_ptr<const T>` |
| Inert app data crossing threads or at rest (session maps, job args, `data` values, config, declaration results, JSON) | **`Datum`** (below) |
| Anything a running VM holds, or values passed to and from intrinsics | **MS2 `Value`**, on the script thread only, and only inside `script` |
| Arguments to MS2 APIs that require them | MS2 `String`, `List<T>`, `Dictionary<K,V>`, converted at the call |

**Why not MS2 `String` everywhere?** It's immutable, `shared_ptr`-based, and safe to share between threads, so it would work. But most engine code deals in bytes: HTTP parsing, file contents, SQL, crypto. Those want `std::string` and `string_view`, and every third-party library speaks them. Converting at the script boundary is one copy, and only for values that actually cross it. Inside `script`, the bindings use MS2 `String` freely, since nearly everything they touch goes into or comes out of a VM.

**Why not `List<T>` and `Dictionary` in engine code?** They're reference types: copying one aliases it, and a default-constructed one has null storage that's allocated lazily, which bites when it's passed by value (see MS2's `CPP_HOST_UPDATE_GUIDE.md`). That's natural in transpiled C#, but a trap in hand-written C++. Use them only where an MS2 API requires them.

**Why not `ValueDict`/`ValueList` for app data?** They hold GC references: they're script-thread only, and invisible to the GC unless rooted. That rules them out for anything stored, cached, or handed to a worker thread.

### `Datum`: inert values in C++

`Datum` is a small immutable tree: null, number, string, list, or map, and nothing else. It's the C++ form of the overview's "inert values" (§2, §3), and the type that the "data is inert" rule is enforced on.

```cpp
class Datum {   // immutable; cheap to copy (shared, refcounted nodes)
public:
    enum class Kind { Null, Number, String, List, Map };
    ...
};
```

- **Codec** (in `base`): `Datum` ↔ bytes, in one canonical serialization (probably JSON, plus a compact binary form if profiling asks for it). Canonical means equal data gives equal bytes, so "save the session only if it changed" is a byte comparison.
- **Bridge** (in `script`): `Datum` ↔ `Value`. `toDatum(Value)` is **the one place** where the inert check happens: it fails on functions, handles, and anything else that isn't inert, and on cycles and excessive depth. Every path from a VM to storage (`session`, `data`, `enqueue` args, `file` writes of values) passes through it, so none of them can implement the check differently.
- Size caps (session 64 KB, job args) are applied to the serialized bytes.

The extra conversion step (`Value` → `Datum` → bytes, instead of `Value` → bytes) costs little at these sizes, and buys a lot: `state`, `appdata`, `jobs`, and `config` never need MS2 at all.

---

## 5. The components

### Foundation

**`base`.** `Datum` and its codec; `Result<T>` and `Error` (MS2 doesn't throw, so the engine shouldn't rely on exceptions for expected failures either); secure random, hashing, HMAC and signed tokens (via libsodium or OpenSSL); clock (injectable, for tests and for schedule logic).

**`partition`.** The four areas from §1, each as its own type, offering only its permitted operations:

| Type | Offers |
|---|---|
| `CodeTree` | list and read; yields `CodeFile` objects. **No write.** |
| `DataDir` | read, atomic write, locked update, append, list, delete; hides `data/.fourfold/` |
| `Dropbox` | append only |
| `ConfigDir` | read only |
| `StateDir` | read and write, engine only |

Path canonicalization and the anti-traversal rule live here and nowhere else. **Nothing else in Fourfold opens files in an app folder directly**; a code-review rule, but easy to check by grepping for raw file APIs outside `partition` and `sqlite`.

The path rules, as built in M1 (several follow raylib-miniscript's `notes/SANDBOXING.md`):

- **`RelPath`** is the only path type the partition operations take. Its segments may not be empty, `.` or `..`, start with `.`, contain control characters or `\`, or be invalid UTF-8. Rejecting dot-names makes `data/.fourfold/` unreachable without a special case.
- **Request paths** are canonicalized by `canonicalUrlPath`: each segment is percent-decoded on its own (so `%2F` can't create a segment, and `%2e%2e` is caught as `..`), repeated slashes collapse, and dot segments are rejected, never resolved.
- **No symbolic links inside a partition.** Files are opened one segment at a time with `openat` and `O_NOFOLLOW`, which leaves no gap between checking a path and opening it. A partition folder itself may be a link (e.g. `code/` to a checkout), resolved once when the app is opened, and refused if any partition ends up inside another.
- **Errors don't help a prober.** Missing files, links, and non-regular files are all `NotFound`; the message says which, for the developer. Script bindings report only "not found". Messages name partition-relative paths, never host paths.

`CodeFile` is constructible only by `CodeTree`. That makes "code is compiled only from the code partition" a type rule: the compiler's entry points take `CodeFile`, not strings or paths (see §6).

**`sqlite`, `net`, `threads`, `log`.** Thin wrappers. `log` provides two unrelated types, `EngineLog` (`private/state/logs/`) and the app's `Dropbox` writer, so engine code can't write security events where the app could forge or bury them (§1).

### Libraries

**`template`.** Two halves sharing one data structure:

- `parseTemplate(CodeFile, kind) → CompiledTemplate | diagnostics`: tokenizes HTML, classifies each field's context (text, quoted attribute, URL start, URL query, script, forbidden), validates page vs. fragment shape, records every `<script>` and `<form method="post">` (for nonce and CSRF injection), and resolves `{{> includes}}`. All the logic in §4's escaping tables lives here.
- `render(CompiledTemplate, FieldSource, RenderOptions) → Html`: walks the template, asks the `FieldSource` for each field's value, and escapes by context. `RenderOptions` carries the CSP nonce and CSRF token to inject.

`FieldSource` is an interface returning a small variant (`null`, `bool`, number, string, `Html`, or `Datum` for JSON-in-script). The script bindings implement it over a `Value` map; pages with no code use an empty one; tests use a literal one. **`template` therefore doesn't depend on MS2**, and the hardest-to-get-right security code can be tested in isolation with tables of inputs and expected outputs.

`Html` is the C++ type for rendered, trusted HTML. Only `render`, `renderMarkdown`, and the `unsafeHTML` intrinsic produce it. In scripts it's wrapped as a handle (§4, Return values).

**`markdown`.** `renderMarkdown(text, profile) → Html`, wrapping md4c with the profile rules, the URL rule (shared with `template`), link rewriting (given a route-lookup callback), and the size cap. Pure, and MS2-free.

**`image`.** The data model of a compiled app, with no logic beyond lookups. Described in §6, since it's the most important seam.

### Subsystems

**`compiler`.** Turns a snapshot of `CodeTree` into an `AppImage` plus a list of diagnostics (errors and warnings, each with file, line, and message). It:

- parses templates and Markdown (via `template` and `markdown`);
- compiles `.ms` files into bytecode (via `script`'s `ScriptCompiler`, which takes `CodeFile`s);
- evaluates declaration files (`access.ms`, `limits.ms`, `outbound.ms`, `schedule.ms`, routes) and per-file declarations (`access =`, `form.x`) in a restricted VM, getting back `Datum`s (via `script`'s `DataEvaluator`);
- builds the route table and computes each route's effective access, limits, and form spec;
- runs the cross-checks: route collisions, unmatched access rules, forms against templates, links between `.md` files, jobs named by the schedule, migration numbering and checksums.

The compiler is, apart from its use of `script`, **a pure function from files to an image**. It touches no stores and no network. That makes it the natural place for golden tests ("this app folder compiles to this route-table dump, with these diagnostics"), and the access report is just a printout of its output.

Internally it's a set of passes over a shared in-progress model, so each feature (forms, Markdown, migrations) adds a pass without disturbing the others.

**`config`.** Reads `private/config/app.ms`, `secrets.ms`, and the optional server config (via `DataEvaluator`), applies defaults, and clamps app values to the server's ceilings. The result is an immutable `AppConfig` struct, made of one sub-struct per area (`SessionConfig`, `BudgetConfig`, `OutboundConfig`, …). **Each component receives only its own sub-struct**, so adding a setting touches `config` and the one component that uses it. Secret values go into a `SecretVault`, which is handed only to `outbound`, `mail`, and `appdata`, and never to `script`.

**`script`.** Everything that knows about MiniScript. Its public face is small:

- `ScriptCompiler`: `CodeFile` → compiled unit (bytecode + the declarations found at top level).
- `DataEvaluator`: runs a declaration or config file in a restricted VM; returns a `Datum`.
- `ScriptHost`: the script thread and its scheduler. Its main call is "run this handler, in this image, with this `RunContext`, and tell me the outcome".

The **`RunContext`** is the seam between the engine and the script bindings. It's a struct of service interfaces and request facts that one run may use:

```cpp
struct RunContext {
    RunKind kind;                      // Request, Job, Migration, Seed
    const AppImage& image;
    const RequestFacts* request;       // null for jobs and migrations
    SessionAccess* session;            // null if there's no session in this kind of run
    DataFiles* data;
    Database* db;
    OutboundClient* http;              // null in migrations
    JobEnqueuer* enqueue;              // null in migrations
    PushPublisher* publish;            // null in migrations, and in CGI mode
    Mailer* mail;
    ...
    Observer* observer;
};
```

The intrinsic modules (`data`, `db`, `http`, `secrets`, `session`, `users`, `mail`, `frag`, `markdown`, `redirect`, `json`, `status`, `enqueue`, `publish`, `flash`, `formState`, `reshow`, `ratelimit`, `unsafeHTML`, …) are **each a separate file**, depending only on `RunContext` and the bridge. A VM gets a module only if its service pointer is non-null. So the per-kind restrictions in the overview (no `http` in migrations, no `session` in jobs, no `job` map outside `code/jobs/`) come from which services the engine supplies, not from checks scattered through the modules.

Return values from handlers are converted into an engine-side `HandlerOutcome` (render with fields, fragment, redirect, JSON, status, reshow, no content, or error) before leaving `script`. Pending side effects (session changes, enqueued jobs, flash) are collected in the `RunContext`'s services and committed, or dropped, by the engine afterward.

**`state`.** Engine-owned stores, each an interface with a SQLite implementation in `private/state/`:

| Store | Holds |
|---|---|
| `SessionStore` | session records (hashed IDs, engine part, app part as bytes), user→sessions index |
| `AuthStore` | users, roles, magic-link tokens |
| `RateStore` | rate-limit counters, used challenges |
| `JobQueue` | pending, running, and dead jobs; schedule state |
| `LockService` | session locks and the schema lock (in memory in standalone mode, file locks in CGI mode) |

Each store's interface deals in `Datum`, bytes, and plain structs. Redis or Postgres implementations for multi-server deployments (§2, §3) slot in behind the same interfaces.

**`appdata`.** The app's own data, in `data/` or a configured database:

- `DataFiles`: Tier 1 operations (get, set, update, append, increment, compare-and-swap), on `DataDir`.
- `Database`: the connection pool and named-query execution. `query()` takes a `NamedQuery`, constructible only from the image, so SQL injection is a type error (§3). Postgres and SQLite are implementations behind one interface.
- `Migrator`: applies a list of migrations from the image, with checksum records, using `script` (via an engine-supplied callback) for `.ms` migrations.
- `BackupService`: consistent snapshots of `data/` and `private/state/`.

**`outbound`.** `OutboundClient::send(OutboundRequest)` where header and URL parts are `std::variant<std::string, SecretRef>`. The client checks the allowlist (from the image) and the host config's tightening, resolves DNS, checks and pins IPs, follows redirects, enforces limits, and **substitutes secret values only as the bytes go out**. A `SecretRef` is a name; the script side wraps it in a handle. The secret's value never exists on the script thread, so no script bug can leak it into a page or a log.

**`mail`.** `Mailer::send(Message)` with structured fields only (no raw headers). Used by `authflow` for magic links and by the `mail` intrinsic.

### Engine

**`pipeline`.** The request path, as a fixed sequence of small stages. Each takes and returns plain structs, so stages can be tested and reordered independently:

1. **Admit:** check `Host`, determine `req.ip` (trusted proxies), canonicalize the path. (From `HttpRequest`.)
2. **Route:** pin the current `AppImage` and `AppConfig` (`shared_ptr<const …>`), take the schema lock shared, and look the route up. Not found → 404.
3. **Identify:** read the session cookie, hash the ID, look up the user in `AuthStore`. The session's app part isn't loaded yet.
4. **Gate:** `policy` decides allow or deny.
5. **Parse:** parse the body under limits, then normalize and validate it against the route's `FormSpec`, giving `req.form`, `req.errors`, and `req.files`.
6. **Dispatch:** a static file is streamed from `CodeTree` or `DataDir`; a template with no code is rendered directly; a handler goes to `ScriptHost`.
7. **Respond:** turn the `HandlerOutcome` into an `HttpResponse`: headers (§8 security headers, CSP nonce, cache rules for restricted routes), cookies, HTMX headers, error pages.
8. **Commit:** save the session if it changed, commit enqueued jobs, release locks; or, on error, discard all of these.

Reserved prefixes (`/__auth/`, `/__fourfold/`, `/__dev/`) are routed to their owners before step 2.

**`policy`.** `PolicyGate::check(RouteEntry, RequestFacts, stores) → Decision`. Access level, CSRF token and `Origin`, rate limits, timing tokens, and challenges. Each check is a separate function that can only add a denial, which mirrors the overview's "rules can only tighten". Table-driven tests cover it thoroughly; it's the piece most worth over-testing.

**`authflow`.** The engine's own pages and flows (§5): magic-link request and redemption, sign-out (one device, or everywhere), fake login in dev. It uses `AuthStore`, `SessionStore`, `Mailer`, and `template` (wrapping the engine-rendered form in the app's `frags/auth/` template). It's routes plus logic, and never runs app code.

**`jobs`.** Evaluates the schedule (cron and the friendlier forms), claims due and queued jobs from `JobQueue`, and runs them through `ScriptHost` with a job `RunContext`, a job budget, and a capped worker count. `fourfold run-jobs` (CGI mode) drives the same code from the CLI.

**`push`.** `PushHub`: channels mapped to subscriber connections, which the I/O loop owns. `publish` posts `(channel, Html)` from the script thread to the hub. Absent in CGI mode, so the `publish` service pointer is null there.

**`lifecycle`.** `AppInstance` owns everything for one running app: the current `AppImage` and `AppConfig`, the stores, and the services. It watches `code/` and `private/config/` (waiting for quiet), runs the compiler, and performs the drain–backup–migrate–swap sequence (§3) using the schema lock. It also refuses unsafe swaps, such as code older than the applied migrations. Compiling, migrating, and serving are separate components; `lifecycle` is the only one that knows the order they happen in.

### Hosts

- **`cli`**: the `fourfold` command and its subcommands (`dev`, `run-jobs`, `backup`, `restore`, `migrate`, `cgi-setup`, `test`).
- **`http-server`**: the standalone server: listening, TLS and ACME, HTTP/1.1 and HTTP/2, connection limits, SSE connections. It produces `HttpRequest`s and consumes `HttpResponse`s.
- **`cgi`**: a thin adapter that builds an `HttpRequest` from CGI environment and stdin, runs the engine once, and writes the response. FastCGI is the same over a socket.
- **`front`**: the multi-app TLS router (§8). Practically a separate program; it shares only `net` and the TLS code.
- **`devtools`**: an `Observer` implementation plus handlers for `/__dev/*`. The core emits events (request started, route matched, policy decision, field filled, query run, outbound call, budget used, session before and after, job enqueued, compile finished, diagnostics), and `devtools` records and displays them. The live-reload script, the error overlay, and the "sign in as…" picker are injected through a render hook. **In production the observer is a no-op**, and `devtools` isn't linked into the request path at all.

Dev mode's *behavioral* differences (unprefixed cookies over plain HTTP, minimal challenge difficulty, fake login enabled, seed data) are settings in `AppConfig`, chosen by `config` when the mode is dev. Components read their settings; they never ask "are we in dev mode?" That keeps `if (dev)` checks from spreading through the code.

---

## 6. The key seams in more detail

Most of the independence comes from five types. If these stay stable, work on either side of each one can proceed without the other.

### `AppImage` (compiler → everything else)

An immutable, self-contained description of one version of the app. It's the only thing the engine knows about the app's code.

```cpp
struct AppImage {
    uint64_t generation;
    RouteTable routes;                    // canonical path → RouteEntry; plus parameterized routes
    TemplateSet templates;                // pages/ and frags/, by name
    ScriptUnits scripts;                  // compiled .ms files (see below)
    std::vector<NamedQuery> queries;
    OutboundPolicy outbound;              // allowlist from outbound.ms
    std::vector<ScheduleEntry> schedule;
    std::vector<Migration> migrations;    // with checksums
    StaticIndex statics;                  // static files in pages/, with content types
    std::vector<Diagnostic> diagnostics;  // warnings that didn't stop the build
};

struct RouteEntry {
    RouteKind kind;                       // Page, Handler, Webhook, Static, Markdown
    TemplateRef page;                     // if any
    HandlerRef handler;                   // file + map + name, if any
    AccessLevel access;                   // effective, already combined
    std::vector<RateRule> limits;         // effective, already combined
    std::optional<TimingRule> timing;
    bool challenge;
    const FormSpec* form;                 // if declared
    std::vector<std::string> accessSources;   // which rules produced `access`, for reports
};
```

Everything the request path needs is already resolved: the route's effective access, its limits, and its form spec. The engine does lookups, never pattern matching or rule combination (§5, Access is resolved per route).

**The script half.** Compiled bytecode is made of MS2 objects, which live in the script thread's GC. So `ScriptUnits` holds opaque `ScriptUnitRef`s, and the bytecode itself is owned by `ScriptHost`, keyed by image generation. When the last request using an old image finishes, `ScriptHost` unroots that generation's bytecode. Everything else in `AppImage` is plain C++ and can be read from any thread.

**Serialization.** CGI mode caches the image in `private/state/`. The plain C++ parts serialize easily. The bytecode needs MS2 support (§1); until then, a CGI cold start recompiles the `.ms` files.

### `HttpRequest` / `HttpResponse` (transports → engine)

Plain structs: method, canonical target, headers, peer address, and the body (fully read, under limits, or a bounded stream for uploads). The response has status, headers, and a body that's either bytes, a file to stream, or an SSE subscription. These hide whether the request came from the standalone server, CGI, FastCGI, the test runner, or `fourfold test`'s in-memory transport.

### `RunContext` (engine → script)

Described in §5. The engine decides which services a run gets; the bindings only use what they're given.

### Store interfaces (engine → state, appdata)

Each store is an abstract class over `Datum`, bytes, and plain structs. Tests use in-memory fakes; production uses SQLite; multi-server deployments could add Postgres or Redis versions.

### `Observer` (core → devtools)

A one-way event interface. The core never reads anything back from it, so dev tools can grow freely without touching the core.

---

## 7. Security rules as types

Several of the overview's guarantees can be made structural in the C++ code itself, not just in the behavior visible to scripts. A component that doesn't know about a rule then can't break it by accident.

| Rule (overview) | Enforced by |
|---|---|
| Code is compiled only from `code/` (§1) | `ScriptCompiler` and `parseTemplate` take `CodeFile`, which only `CodeTree` creates. No API compiles a string. |
| SQL runs only from `code/queries/` (§3) | `Database::query` takes `NamedQuery`, which only the compiler creates. |
| Persistent data is inert (§2, §3) | Storage APIs take `Datum`; `toDatum(Value)` is the only route in. |
| Fields are escaped; HTML comes only from templates or `unsafeHTML` (§4) | `Html` is a distinct type; `FieldSource` returns strings and `Html` as different variants. |
| Secrets go only to their hosts (§6) | `SecretRef` in requests; values held only in `SecretVault`, never on the script thread. |
| Engine logs are separate from the dropbox (§1) | `EngineLog` and `Dropbox` are unrelated types. |
| Access is checked before any handler (§5) | `ScriptHost` runs a handler only given a `GateApproval` token, which only `PolicyGate` creates. |
| Rules can only tighten | Combination code (access, limits, config ceilings) builds from "deny" toward "allow" one way only, in one function per kind. |

---

## 8. How a request flows

A `POST /guestbook/sign` from a signed-in visitor, with HTMX:

1. `http-server` parses the request under its limits and hands an `HttpRequest` to `Engine::handle`.
2. `pipeline` admits it, pins image generation 42, finds the `RouteEntry` (kind Handler; `post.sign` in `pages/guestbook.ms`; access `login`; limits `5/minute`, `50/day`; a `FormSpec`).
3. It reads the session cookie and looks up the user in `AuthStore` (on the worker pool).
4. `policy` checks login (yes), the CSRF token and `Origin` (valid), and the rate limits (`RateStore`, within limits). It returns a `GateApproval`.
5. `pipeline` parses and validates the form, producing `req.form` and `req.errors` as `Datum`s.
6. `ScriptHost` creates a fresh VM on the script thread, loads generation 42's bytecode, builds a frozen `req` from the request facts, installs the modules this `RunContext` allows, and calls `post.sign`.
7. The handler calls `data.append`. The `data` module converts the entry with `toDatum`, posts the append to the worker pool, and parks the VM. Other VMs run meanwhile. When the append finishes, the VM resumes.
8. The handler returns `frag("entry", entry)`. The `frag` intrinsic renders `frags/entry.html` with `template::render`, and returns an `Html` handle; `ScriptHost` converts the return value to `HandlerOutcome::Fragment`.
9. `pipeline` builds the `HttpResponse` (headers, CSP), then commits: the session wasn't touched, so nothing is saved; no jobs were enqueued.
10. `http-server` sends the response. If dev mode is on, `devtools` has meanwhile recorded the events from each step for the request inspector.

---

## 9. Source layout

```
fourfold/
  src/
    base/          Datum, codec, Result, crypto, clock
    partition/     CodeTree, DataDir, Dropbox, ConfigDir, StateDir
    platform/      sqlite, net, threads, log
    template/
    markdown/
    image/
    compiler/      one file per pass
    config/
    script/        host/, bridge/, modules/ (one file per intrinsic module)
    state/
    appdata/
    outbound/
    mail/
    engine/        pipeline/, policy/, authflow/, jobs/, push/, lifecycle/
    devtools/
    hosts/         cli/, http-server/, cgi/, front/
  tests/           mirrors src/; plus apps/ (sample app folders for golden compiler tests)
  third_party/     md4c, sqlite, …   (MS2 itself from ~/svnrepo/miniscript2)
```

Each component directory has its public headers in `include/<component>/` and its internal files beside them. A component's tests use only its public headers, plus fakes of the interfaces below it. Each component is its own static library, and only components that declare a dependency on it can see its `include/` folder, so the build itself rejects a dependency that points upward or sideways. The build system and its conventions are described in [building.md](../building.md).

Third-party pieces: llhttp for parsing HTTP/1.1 (chosen in M1). Likely, still to be decided: SQLite; md4c; nghttp2 for HTTP/2; OpenSSL or BoringSSL for TLS; libsodium for crypto; an ACME client or a small one of our own. The event loop is our own (`platform`'s `EventLoop`, over kqueue or epoll): the script thread needs little from it beyond readiness, timers, and a cross-thread wakeup.

---

## 10. What this buys

Some examples of changes, and what each one touches:

| Change | Touches |
|---|---|
| A new template context rule | `template` (and its tests) |
| A new form field type | `compiler` (the `FormSpec` pass), `pipeline` (the parse stage), and the form validator |
| Sessions in Redis | a new `SessionStore` implementation; `config` |
| A new intrinsic module, e.g. `mail` | one file in `script/modules/`, one pointer in `RunContext`, the service itself |
| HTTP/2 | `http-server` only |
| A new dev panel | `devtools` only, plus any new `Observer` event |
| Fields in Markdown (still open, §4) | `markdown`, `template`, and one `compiler` pass; not the engine |
| Per-thread GC in MS2 | `ScriptHost` internals |
| Passkeys | `authflow`, `AuthStore`, `config` |

---

## Open questions

- **Where the compiler's MS2 work runs.** The compiler needs MS2 to compile `.ms` files and evaluate declarations, but MS2 work must happen on the script thread. Options: run compile steps on the script thread in slices (simple, but a big compile delays requests); compile in a separate short-lived process that hands back a serialized image (isolated, and the same path CGI uses, but needs bytecode serialization); or a second GC heap once MS2 supports per-thread GCs.
- **Per-VM memory budgets** with a shared GC. Maybe per-VM allocation accounting in MS2; otherwise a process-level limit plus restarting a worker process that exceeds it, which favors the several-processes model.
- **Event loop and HTTP stack.** Asio vs. libuv vs. a small hand-rolled loop. HTTP/2 and TLS tilt toward existing libraries.
- **`Datum` serialization format.** JSON is debuggable and matches `json()`; a binary format is smaller and faster. Possibly JSON first, behind the codec, so it can change later.
- **How much of the engine should be shared with `fourfold test`.** Ideally the whole pipeline, with an in-memory transport, fake stores, and a fake clock.
- **Whether `front` should be a separate binary**, or a mode of the main one.

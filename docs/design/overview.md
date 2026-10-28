# Fourfold: A Sandboxed Web Backend for MiniScript — Design Notes

*Status: early design, big-picture decisions only. Started 2026-10-21.*

## Name

The product is **Fourfold**, after its four partitions (code, data, dropbox, private), and because it should suggest strength and structure rather than smallness. It's intended for serious work, so the name deliberately avoids a second "mini". The command is `fourfold` (e.g. `fourfold dev ./myapp`).

Suggested attribution line for sites: **"Built with Fourfold, powered by MiniScript."**

Other API names (`frag`, `publish`, `flash`, etc.) and API details are illustrative, not final.

(Names considered and set aside: Tiffin, because ShipTiffin is a self-hosted app platform launched under that name in October 2026; MiniScript Server Pages, which implies code embedded in pages; MiniWeb and similar, which sound diminutive; MiniScript Backend and Server-Side MiniScript, both descriptive but less distinctive. The latter also invites the SSMS acronym.)

## Goals

- **Secure by design.** Common web vulnerabilities, especially the classic PHP ones, should be structurally impossible, not just discouraged.
- **Easy and fun.** You can start with pure HTML/CSS and zero code, then add small features (a visitor count, a guestbook) with a small amount of MiniScript. The edit → save → see-it loop should be immediate.
- **Scales up.** The same model should hold up as an app grows into something larger, backed by a real database.

The system is **server-side**: MiniScript runs on the server (built on the MiniScript 2 VM in `~/svnrepo/miniscript2`). The browser gets HTML/CSS plus a small, generic JS library; app developers aren't expected to write JavaScript.

---

## 1. Sandboxed partitions

Each app's files are split into four partitions, which are separate directory trees. The first three are reachable from MiniScript code; the fourth is reachable only by the engine itself:

| Partition | Contents | Engine access | MiniScript access |
|---|---|---|---|
| **Code** `code/` | MiniScript scripts, HTML/JS/CSS templates, SQL queries, static assets and data | Read only | Read only (the only place code is compiled from) |
| **Data** `data/` | App data files, SQLite databases, user uploads | Read + write | Read + write |
| **Dropbox** `dropbox/` | The app's own log files and other append-only output | Write / append only | Write / append only |
| **Private** `private/config/` | App config, secrets (see §10) | Read only | None |
| **Private** `private/state/` | Auth store, session store, rate-limit counters, compiled-app cache, engine logs | Read + write | None |

```
myapp/
  code/
  data/
  dropbox/
  private/
    config/
    state/
```

The private partition is split because its two halves are written by different parties. `config/` is written by the developer or admin and is read-only to the engine, so even an exploited engine bug couldn't **rewrite its own policy**: add an admin, widen the outbound allowlist, or bind a secret to a new host. `state/` is the engine's own working storage.

**Core rule:** *anything that gets executed or interpreted as code (MiniScript, templates, SQL) lives in the code partition; the data area only ever holds inert values.*

- The engine **only compiles code from the code partition.** No intrinsic (`import`, `run`, any form of `eval`) may compile a string or file from anywhere else. So MiniScript code can never alter its own code, or write out new code and then run it. This closes PHP's worst holes: remote/local file inclusion, upload-then-execute, and self-modifying code.
- The existing MiniScript path sandbox keeps paths from escaping their partition (e.g. `../` traversal).
- The dropbox being write-only means a compromised app can't read or tamper with its logs, so they can serve as an audit trail.
- **The engine's own security-relevant logs** (access log, outbound log, sign-in events, errors) go to `private/state/logs/`, **not** the dropbox. App code can append to the dropbox, so it could otherwise write fake engine log lines, or noise to bury real ones.

### OS-level enforcement (belt and suspenders)

On top of the engine's own checks, the partitions are protected with OS file permissions:

- The engine runs as its own OS user (e.g. `fourfold`), not as the developer.
- `code/` is owned by the developer; the engine user has read-only access. Even a fully compromised engine can't modify code.
- `data/` is readable and writable by the engine user (e.g. through a shared group).
- `dropbox/`: true write-only access is hard on Unix, because whoever owns a file can `chmod` it and read it back. Options:
  - Linux `chattr +a` (append-only) on pre-created files.
  - A tiny separate logger process, running as a different user, that the engine sends log lines to. This is the most robust option and also allows rate-limiting.
  - Engine-only enforcement (acceptable for local dev, and probably "good enough" for version 1).
- `private/config/` is owned by the developer (or admin); the engine user has read-only access.
- `private/state/` is readable and writable by the engine user only.
- On a server hosting several apps, each app's engine runs as a separate OS user.
- The engine binary and the optional server config (see §10) live outside every app's partitions, owned by the admin.
- In CGI mode on shared hosting, none of this is available; see [Hosting modes](#hosting-modes).

### Developer access

The developer can read, edit, move, and delete files in all the partitions, both locally and on the server. Locally, the partitions are simply folders.

---

## 2. Execution model: a fresh VM per request

Each HTTP request runs in a **fresh MiniScript VM**, discarded when the request finishes.

- Between requests, the only things that persist are the read-only code and inert data (the `data` and `session` stores). No in-memory state can build up or linger.
- **Startup cost:** the code partition is compiled once into frozen bytecode shared across all request VMs. Each request only allocates its own locals and globals.
- **Resource budgets:** each request gets limits on instructions, memory, and wall-clock time, set in the host config. An infinite loop produces a friendly error, not an outage.

### Three scopes of state

| Scope | Lives for | Holds |
|---|---|---|
| locals / globals | one request | anything (including functions) |
| `session` | one visitor's session | inert values only |
| `data` | permanently | inert values only |

- **`session`** is a size-capped map of inert values (numbers, strings, lists, maps), managed by the engine. Because it can't hold code, it survives restarts and deploys safely. See [Session storage](#session-storage) below.
- Requests within a single session that use `session` are queued, so a user can't race with themselves (double-clicks, several tabs).
- The `session.` prefix is deliberate: it marks clearly what lasts beyond the current request.

**Considered and rejected: a VM per session** (session state as ordinary globals). Main reasons:
- Deploys and hot reload: live session VMs would hold stale functions and stale data shapes.
- State is lost on restart or crash unless VM heaps are snapshotted, which is hard with closures and prototypes.
- Memory-exhaustion DoS: every visitor or bot ties up a VM until timeout.
- "Globals persist… until they don't" is a trap for beginners.
- It weakens the "only inert data persists" security property, and requires sticky sessions to scale.

"Conversational" straight-line flows (Seaside/Arc style) might be worth revisiting later as a separate, opt-in mode.

### Session storage

#### Where sessions live

Sessions are kept in an **engine-owned store on disk (SQLite) with an in-memory cache**, next to the auth store in `private/state/`, **outside `data/`**. App code reaches only the current visitor's `session`, never the store itself, so it can't list or read anyone else's. Redis or Postgres can be added later as a host-config option for multi-server deployments.

Rejected:
- **In memory only:** lost on every restart, breaking the promise that sessions survive restarts and deploys.
- **Signed or encrypted cookies** (Rails/Flask style): about a 4 KB limit; sessions **can't be revoked**, so server-side sign-out is impossible; and an old cookie can be **replayed** to restore old state (e.g. a "coupon not yet used" flag).

#### Session ID

- 128+ random bits from a cryptographically secure generator, sent only in the session cookie (see §5 for cookie attributes).
- **The server stores a hash of the ID, not the ID itself.** If the session store leaks (a backup, a disk image), the attacker gets contents but no IDs usable to hijack live sessions.

#### What a session record holds

- **The engine's part:** user ID (if signed in), created and last-used times. App code can't touch it. (The CSRF token isn't here: it comes from a cookie of its own; see §5, [CSRF protection](#csrf-protection).)
- **The app's part:** the `session` map.

`req.user` is built from the user ID by **looking it up in the auth store on every request**. Roles are never cached in the session, so revoking a role or deleting a user takes effect immediately.

#### Load lazily, lock lazily, save only if changed

- **The engine loads the session on first access** to `session`. A request that never touches it doesn't load it and doesn't wait for any lock. That covers most static pages and many renders. *(As built in M5: decided per route at compile time, since MS2 can't detect a map's first access. A route loads and locks the session if its code, or a lib it imports, names `session` or `flash`, or its page has a `{{flash}}` field. See [m5-results.md](m5-results.md).)*
- **First access also locks the session** until the request ends. This is the per-session queuing, applied only to requests that actually use `session`. There's only one lock, so deadlock isn't possible, and request budgets limit how long one tab can make another wait.
- At the end of the request, the engine serializes the map and **writes it only if it changed**.
- **"Last used" is refreshed at most every few minutes**, not on every request, so sliding timeouts don't cost a write per page view.
- Anonymous sessions are created only when code first *writes* to `session`; the session cookie is set on that response.

#### Size cap and invalid contents

- Default cap: about **64 KB serialized**, set in the host config.
- The check happens at save time, **before the response is sent**. If the map is too big, or contains anything that isn't inert (a function, a handle), **nothing is saved and the response becomes a 500.** Dev mode shows the overlay (e.g. "session exceeds 64 KB: largest keys are…"); production logs to the engine log.
- It's a hard failure on purpose. Silently not saving would leave the visitor's state quietly out of sync with what the page showed them.

#### Timeouts

Defaults, configurable in the host config:

| | Idle timeout | Absolute limit |
|---|---|---|
| Anonymous | 24 hours | 7 days |
| Signed in | 14 days | 30 days |

Long signed-in sessions suit magic-link sign-in, where signing in again means a trip to your email. The absolute limit keeps a stolen cookie from lasting forever.

**Limits against bots:** a cap on anonymous sessions per IP and in total. When the cap is reached, the oldest anonymous sessions are evicted first. Signed-in sessions are never evicted for this.

#### Sign-in and sign-out

- **Sign-in:** the session gets a new ID; the anonymous map carries over (e.g. a shopping cart).
- **Sign-out:** the whole session is deleted, including the app's map, which is what people expect on a shared computer.
- **Sign out everywhere:** the engine keeps an index from user to sessions, so a "sign out of all devices" action, or an admin disabling a user, can delete all of that user's sessions.

#### Flash messages

Redirect-after-POST almost always wants a one-time message. It goes in one reserved key of the session:

```
session.flash = "Thanks for signing!"
return redirect("/guestbook")
```

The next page shown with a `{{flash}}` field (or `{{flash.x}}`) displays `session.flash`, and showing it removes it from the session. Otherwise it's an ordinary session value: code can read, change or remove it, and it can be a map (`{"text": "Saved.", "kind": "ok"}`, shown with `{{flash.text}}` and `{{flash.kind}}`). If `render` returns its own `flash` field, that's shown instead, and `session.flash` is still removed.

*(Decided after M5: M5 built a `flash "…"` function that kept the message in the engine's part, where code couldn't see it. A plain session key is one less concept, and lets code read and shape the message.)*

#### Dev mode

- The request inspector shows the session before and after each request, including its size against the cap.
- Sessions can be viewed, edited, and cleared from the dev tools.
- Each fake-login user gets its own session, so switching between "admin" and "visitor" doesn't mix them up.

### Background jobs

No VM outlives its request, but real apps need work done outside one: nightly cleanup, digest emails, expiring old data, polling a feed, or sending a welcome email without making the visitor wait. Jobs provide this without giving up the per-request model: **a job is just another handler, run by the engine in a fresh VM**, and the only thing that waits between runs is inert data.

#### Defining jobs

Job handlers live in `code/jobs/`, in a `job` map that the engine creates before each file runs, like `get` and `post`:

```
// code/jobs/mail.ms
job.sendWelcome = function(args)
    u = users.get(args.user)
    if u == null then return    // the account is gone; nothing to do
    return mail.send(u.email, "welcome", {"name": u.name})
end function
```

- Job names must be unique across `code/jobs/`; a duplicate is a compile error.
- **No URL ever reaches a job.** `job` maps are only allowed in `code/jobs/`, and page `.ms` files can't define one. A job runs only when scheduled, when enqueued by code, or from the dev tools.
- A job gets a frozen `args` map instead of `req`. There's no `session` and no `req.user`: a job acting for a user is passed the user's `id` and looks the user up when it runs, so roles are current (the same rule as `req.user`, §2).
- `data`, `db`, `http`, `secrets`, and `publish` work as they do in requests. A scheduled feed poll can `publish` new items straight to SSE subscribers.
- **Return value:** an `error` value means the job failed and is retried (below). Anything else counts as success.

#### Enqueuing

```
post.signup = function(req)
    ...
    enqueue "sendWelcome", {"user": id}
    enqueue "remind", {"user": id}, {"delay": 3 * 24 * 60 * 60}   // in 3 days
    return redirect("/welcome")
end function
```

- **Arguments must be inert**, checked when `enqueue` is called; a function or handle in `args` is an error. So the queue, like `session`, survives restarts and deploys safely.
- The queue lives in an engine-owned SQLite store in `private/state/`, not in `data/`, so app code can't read or rewrite pending jobs.
- **Jobs enqueued during a request are committed only if the request succeeds.** If the request ends in an error, they're dropped (with a dev-mode note), just as the session isn't saved. A failed signup shouldn't send a welcome email.
- Optional `{"key": "digest:" + id}` makes the job unique while it's pending: enqueuing the same key again does nothing. That covers "send at most one digest" without app-side bookkeeping.

#### Scheduling

Schedules are declared in `code/schedule.ms`, evaluated at compile time as data, like `access.ms`:

```
schedule = {}
schedule["cleanup"]   = "daily 03:00"
schedule["digest"]    = "weekly mon 08:00"
schedule["pollFeeds"] = "every 15m"
schedule["report"]    = "0 6 1 * *"           // plain cron syntax also works
```

- A schedule naming a job that doesn't exist is a compile error.
- Times are in the app's configured time zone (default UTC).
- **A scheduled job never overlaps itself.** If the previous run is still going when the next one is due, that tick is skipped.
- **Missed runs run once, not many times.** After downtime, each overdue scheduled job runs once at startup, not once per missed tick.

#### Running, failing, retrying

- **At-least-once delivery.** A job can run more than once (e.g. if the engine stops mid-run), so the docs teach that jobs should be safe to repeat. The `key` option and atomic `data` operations make that easy.
- A failed job is retried with exponential backoff, up to a limit (default 5 attempts). After that it's marked **dead**: kept with its last error, written to the engine log, and shown in the dev tools.
- **Jobs have their own budget**, set in the host config and separate from request budgets. The default wall-clock time is longer, e.g. 60 seconds.
- **Jobs can't starve requests:** they run on a separate, capped worker pool (default: a couple of workers).
- **Deploys:** a job runs on whichever version of the app is current when it starts. If a deploy removes a job that still has queued entries, the compile step warns, and those entries go dead when they come up rather than vanishing silently.

#### Dev mode

- A **jobs panel**: the queue, recent runs and their results, dead jobs (with retry and discard buttons), the schedule with next run times, and a "run now" button for any job.
- The request inspector shows which jobs a request enqueued.
- Scheduled jobs run in dev just as in production; a toggle pauses them.

#### CGI mode

There's no long-running process in CGI mode, so the host's cron runs `fourfold run-jobs ./myapp` every minute. It runs whatever scheduled jobs are due and works through the queue until a time limit is reached. Enqueued jobs therefore wait up to a minute.

#### Still open

- Whether jobs need a way to enqueue follow-up jobs in a chain, and whether a job's enqueues commit only when it succeeds, like a request's.
- Multi-server deployments need the queue in Postgres (or similar) instead of SQLite, with workers claiming jobs safely. Same host-config option as for sessions.
- In CGI mode without cron access, whether to fall back to running due jobs after a request's response has been sent.

---

## 3. Data storage

Three tiers, all following the core rule (data is inert; executable things live in `code/`).

### Tier 1: plain files in `data/`

- **Atomic writes:** write to a temp file, then rename it over the original, so readers never see a half-written file, even if a request is killed by its budget.
- **Locked read-modify-write:** `data.update(name, @f)` takes an exclusive lock, passes the current value to `f`, and writes back the result. The request budget limits how long the lock can be held.
- **Append** is offered separately, since appends are naturally safe to run concurrently.
- **Value serialization that can't produce code:** a save/load (or JSON) facility for numbers, strings, lists, and maps that *refuses functions*. Loading data can never produce something callable (no equivalent of PHP's `unserialize` hole).
- Shared counters etc. should use atomic operations (`data.increment`, `data.append`, `data.compareAndSet`), so that the simple way is also the correct way.

*As built in M5* ([m5-results.md](m5-results.md)): a name like `"guestbook"` or `"polls/lunch"` holds either a value (`data/guestbook.json`, read with `data.get` and replaced whole with `data.set` or `data.update`) or a log (`data/guestbook.jsonl`, added to with `data.append` and read with `data.get` as a list). The module also has `data.increment`, `data.compareAndSet`, `data.delete`, `data.exists` and `data.names`. Failures come back as error values.

*Added in M5.1* ([m5.1-results.md](m5.1-results.md)): a `file` module reaches any file in `data/`, whole, as text (`file.readLines`, `file.writeLines`) or bytes (`file.loadRaw`, `file.saveRaw`, with Fourfold's own `RawData`), plus `exists`, `info`, `children`, `makedir`, `delete` and the path helpers. Paths are relative to `data/` and can't leave it. Writes are atomic and take the file's lock, the same one `data` takes, so `data` is a convenience layer over files. Nothing in `data/` is ever served by URL.

### Middle tier: SQLite

A single file in `data/`: real transactions, no separate server. Probably the right choice for most apps that outgrow plain files. It uses the same query API as Tier 2, so moving to Postgres later is mostly a configuration change.

### Tier 2: PostgreSQL or a key-value store

- **The host owns connections.** Credentials, the DB URL, and a least-privilege DB role per app live in the host config. The app only sees an already-connected `db` handle and never holds credentials. The host also owns connection pooling, which per-request VMs need anyway.
- **Named queries in the code partition:** SQL lives in files like `code/queries/findUser.sql` and is called by name, with values always bound as parameters:

  ```
  rows = db.query("findUser", [email])
  if rows isa error then return showError(rows)
  ```

  The engine only runs SQL from the code partition, which makes SQL injection structurally impossible. It also extends the code/design separation: someone who knows SQL can review or tune `queries/` without reading app logic.
- Dynamic SQL (user-chosen sort column, optional filters) gets a narrow, clearly marked escape hatch, such as identifier slots checked against an allowlist declared alongside the query. Never raw strings.
- A key-value store is likewise a host-provided handle with a namespace per app, offering the same atomic operations.

### Schema changes and migrations

Apps outlive their first data layout. Tables gain columns, data files change shape, and the change has to reach production without breaking the site or the data. The approach: **migrations are code, kept in `code/`, applied forward only, as part of deploying.**

#### Migration files

Migrations live in `code/migrations/`, numbered, and run in order:

```
code/migrations/
  001_create_guestbook.sql
  002_add_entry_time.sql
  003_backfill_entry_time.ms
  004_add_likes_index.sql
```

- **`.sql` migrations** run against the app's database (SQLite or Postgres). They're ordinary SQL files: SQL from the code partition, like named queries.
- **`.ms` migrations** handle what SQL can't: reshaping Tier 1 data files, or backfills that need logic. Each defines `migrate = function()`; returning an `error` value fails the migration. They run in a fresh VM like a job, with `data`, `db`, and file access, but **no `http`, `mail`, `publish`, or `enqueue`**: a migration should change data and nothing else, so that a failed migration can be undone completely.
- Numbers must be unique; a duplicate is a compile error. Gaps are fine.
- **Forward only; there are no "down" migrations.** Down migrations are rarely tested and often wrong. Undoing a migration means restoring the backup taken just before it (below), or writing a new forward migration.
- **An applied migration can't change.** The engine stores each applied migration's checksum; if the file is edited afterward, that's a compile error ("003 was changed after it was applied; write a new migration instead"). Dev mode offers "redo the last migration" for the edit-and-retry loop while a migration is being written.
- **A new migration numbered below one already applied** (typically from merging two lines of work) is a compile error: renumber it.
- A fresh install runs every migration from the start, so the migrations folder *is* the schema's history. This also means `.ms` migrations that call named queries can break later if those queries change. The docs recommend keeping migrations self-contained.

#### Where the record lives

The record of applied migrations is stored **with the data it describes**, so restoring a backup brings back the matching record:

- For the database: an engine-owned `fourfold_migrations` table inside it (SQLite or Postgres). It's not exposed through named queries.
- For Tier 1 files: an engine-reserved `data/.fourfold/` folder, which the path sandbox hides from app code.

#### Applying migrations: drain, migrate, swap

Pending migrations are applied as part of the whole-app compile and swap (§8):

1. The new version compiles. If it fails, nothing else happens; the old version keeps serving.
2. If it has no pending migrations, it's swapped in as usual. Most deploys stop here.
3. Otherwise the engine **stops starting new requests and jobs**. New requests wait in a queue instead of failing.
4. It **waits for in-flight requests and jobs** on the old version to finish (each is bounded by its budget).
5. It **takes a backup** (below).
6. It **runs the pending migrations in order.** Each `.sql` migration runs in its own transaction; SQLite and Postgres both support transactional schema changes.
7. **On success:** the new version is swapped in and the queued requests proceed on it.
   **On failure:** the database and data files are restored to the pre-migration backup, the old version resumes, and the error goes to the engine log (and the dev overlay). The site never runs old code against a new schema, or new code against an old one.

- **No request on the old version ever sees the new schema**, because there are none left running when migration starts. That's the simplest rule that's always correct, and the pause is a second or less for typical hobby-scale migrations.
- **If a migration runs long**, queued requests wait up to a configured limit (e.g. 30 seconds), then get a 503 with `pages/503.html` if it exists ("down for maintenance; back shortly"). Long migrations (an index on a big table) get their own generous budget, separate from requests and jobs.
- **The same lock works in every hosting mode.** Every request and job holds a shared "schema lock" while it runs; the migrator takes it exclusively. In standalone mode this is in memory; in CGI mode it's a file lock in `private/state/`, so independent CGI processes coordinate too. In CGI mode, the first request that sees new code runs the migrations while later ones wait.
- **Rolling back code past a migration is refused.** If the database is at migration 7 and the new version only knows 1–5 (an older version of the code was deployed), the swap is refused with a clear error, since old code would run against a schema it doesn't understand. The fix is to restore a backup, or deploy code that includes the migrations. An explicit override exists for emergencies.

**Considered: online ("expand/contract") migrations**, where migrations run while the old version is still serving and must therefore be compatible with both old and new code (add a column in one deploy, start using it in the next, drop the old one in a third). This avoids the pause but asks for discipline most hobby apps don't need. It may come later as an opt-in deploy mode for large apps.

#### What migrations don't cover

- **Sessions and queued jobs aren't migrated.** A `session.cart` or job `args` written by old code keeps its old shape after a deploy. Code should tolerate older shapes (or check a version field). A config option clears all anonymous sessions on a deploy that ran migrations.
- **Postgres in a multi-server deployment** needs a migration lock in the database itself (an advisory lock), so only one server migrates; the others wait for it.

#### Backups

Migrations depend on reliable backups, and so does everything else.

- **`fourfold backup ./myapp`** makes a consistent snapshot of `data/` and `private/state/`. SQLite files are copied with SQLite's online backup API, not a file copy, so a write in progress can't corrupt the copy. Data files are copied under a brief write lock. Sessions can optionally be left out.
- **Automatic backups:** before every migration batch (always), and on a schedule if configured, with a retention limit. They go to `private/state/backups/` by default, or a configured directory **outside every partition**. A backup holds the auth store, so it must never end up somewhere app code or the web can reach.
- **`fourfold restore`** puts a snapshot back while the app is stopped (or paused, as for migrations).
- **Postgres** is backed up by its own tools (`pg_dump`, or the provider's backups). Fourfold records the migration state but doesn't own Postgres backups. Without a Fourfold-made backup, a failed Postgres migration is undone by its transaction alone, which covers `.sql` migrations; an `.ms` migration on Postgres runs its database work in one transaction for the same reason.

#### Dev mode

- Pending migrations are applied automatically after each successful recompile (after the usual wait for files to stop changing), with a backup each time.
- A **migrations panel** shows which migrations are applied, pending, or failed, with "redo last" and "reset dev data" (recreate the database from scratch and rerun every migration).
- **Seed data:** if `code/seed.ms` exists, it runs after "reset dev data" (and on a fresh dev database) to fill in sample content and test users' data. It never runs in production.

#### Still open

- **SQL dialects.** SQLite and Postgres differ, so "moving to Postgres is mostly a configuration change" (Middle tier, above) isn't quite true for migrations. Options include per-dialect variants (`004_x.sqlite.sql`, `004_x.pg.sql`) or a restricted portable subset.
- Whether production should apply migrations automatically on deploy (the default above) or optionally wait for an explicit `fourfold migrate`.
- How `.ms` migrations should reach the database without depending on named queries that may change later.

---

## 4. Pages, templates, and code

### Separating design from logic

Unlike PHP, code is **not** embedded in HTML. HTML templates contain named **template fields**, and MiniScript code lives in separate files and fills them in. This adds a little cognitive load in the simplest cases, but scales much better and lets designers own the HTML/CSS.

To keep that load low, a page and its code are paired by naming convention: `pages/guestbook.html` automatically pairs with `pages/guestbook.ms`.

### Progressive path

1. Start with pure HTML/CSS (or Markdown): put `about.html` (or `about.md`) in `pages/` and it's served at `/about`. No code required.
2. To add a feature, add a template field to the HTML and write a small `.ms` file next to it.
3. Larger apps add explicit routes, fragments, real-time updates, and databases.

### Routing: hybrid (decided)

- **File-based routing for pages:** `pages/about.html` (+ optional `about.ms`) is served at `/about`. So is `pages/about.md` (see [Markdown](#markdown)).
- **Static files live in `pages/` too:** anything in `pages/` that isn't `.html`, `.md`, or `.ms` is served as a static file at its path, e.g. `pages/members/photos/beach.jpg` at `/members/photos/beach.jpg`. Images stay next to the pages that use them, and folder access rules cover both.
- **Handlers by convention:** `POST /guestbook/sign` calls `post.sign` in `pages/guestbook.ms`. See [Handler model](#handler-model).
- **One canonical URL per page, so relative links work.** A folder's index page is canonical *with* a trailing slash (`pages/docs/index.html` at `/docs/`); every other page and file is canonical without one (`/docs/intro`). The other form redirects to the canonical one, so a relative link like `foo.png` always resolves against the page's own folder, whichever form the visitor typed. A request for a page by its file name (`/docs/intro.html`, `/docs/index.html`) also redirects to its URL, so links into a site converted to Fourfold keep working. There are no directory listings.
- **An explicit routes table as fallback** for URLs that don't fit the convention: URL patterns like `/user/{id}`, handlers shared by several pages, API-style endpoints.

All of these map URLs to handler functions and templates in the code partition.

### Pages vs. fragments

Templates come in two kinds, kept in separate folders:

| | `code/pages/` | `code/frags/` |
|---|---|---|
| Content | Complete documents, `<html>` … `</html>` | Pieces of HTML (a list item, a table row, a header) |
| Routable? | Yes: file-based routing serves them at a URL | **No**: only ever rendered by code |
| Used by | The router, for full-page responses | Handlers (fragment responses, SSE pushes) and pages (filling fields) |

- The separation is needed for routing, not just tidiness: anything in `pages/` gets a URL, and a fragment served on its own would be a broken half-page (and might show markup that was never meant to be seen out of context).
- The engine validates each kind at compile time: a page must be a complete document, and a fragment must not contain `<html>`. Misplaced files cause a clear compile error.
- Code uses a different call for each, e.g. `frag("entry", entry)` renders `frags/entry.html`. The name is resolved within `frags/` only.
- The same fragment can be reused for both the first render and later updates: a page fills `{{entries}}` with `entry` repeated over the stored list, and the `/guestbook/sign` action returns one more `entry` to append. The HTML for a guestbook entry lives in exactly one place.
- Shared pieces like a site header or footer are also fragments.

### Template syntax and safety

#### No logic in templates

Templates contain **only fields**: no `if`, no loops, no expressions, no filters. All logic lives in code:

- **Loops:** `frag("entry", list)` renders a fragment once per item.
- **Conditionals:** code chooses what goes in a field.

```
nav = frag("signInLink")
if req.user then nav = frag("hello", req.user)
return {"nav": nav, "entries": frag("entry", data.get("guestbook", []))}
```

Designers learn one thing, there's no template language to secure, and logic can't creep into the HTML over time. The cost is more small fragment files, and trivial decisions (e.g. which nav item gets an "active" class) move into code.

#### Fields: `{{name}}`

- `{{name}}` marks where a value goes. Spaces inside are optional: `{{ name }}`.
- **Dotted paths** are allowed: `{{user.name}}` looks up `name` in the `user` map. That's the only operation: no calls, indexing, or math. Formatting (dates, currency, etc.) is done in code.
- A literal `{{` is written `\{{`.
- **There is deliberately no raw-HTML syntax** (no equivalent of Mustache's `{{{x}}}`). Raw HTML only comes from code, via the explicit, grep-able `unsafeHTML`.
- (Considered: Thymeleaf-style "natural templates", e.g. `<span data-field="visits">42</span>`, which preview with sample content. Rejected as wordier, and attributes would need a second mechanism.)

#### Where fields are allowed: contextual escaping

The engine parses every template at compile time and classifies each field by context; Go's `html/template` is the model. **A field anywhere unsafe or ambiguous is a compile error**, so a bad template never deploys.

| Context | Example | Treatment |
|---|---|---|
| Text | `<p>{{msg}}</p>` | HTML-escaped; fragments inserted as HTML |
| Quoted attribute | `title="{{tip}}"` | Attribute-escaped |
| URL attribute, at start | `href="{{link}}"` | Must be http(s), mailto, or a relative URL; `javascript:` etc. are blocked |
| URL attribute, after `?` or `=` | `href="/more?page={{n}}"` | Percent-encoded |
| Inside `<script>` | `const cfg = {{config}};` | Encoded as a JSON literal (maps and lists work) |
| Unquoted attribute, tag or attribute name, event handler (`onclick`), `<style>`, HTML comment | | **Compile error** |

The `<script>` case gives a safe way to pass data to the occasional bit of JavaScript without ever building JS strings.

#### How values become text

| Value | In text | In an attribute |
|---|---|---|
| string | escaped | escaped |
| number | MS2's standard number formatting | same |
| `null` | empty | **attribute omitted** (if the field is the attribute's entire value) |
| rendered HTML (from `frag` or `markdown`) | inserted as HTML | compile/render error |
| map / list | error (except inside `<script>`, as JSON) | error |

- **Boolean attributes** (`checked`, `disabled`, `selected`, `hidden`, …; the engine knows the standard list) are on/off switches: `<input type="checkbox" checked="{{agreed}}">` renders `checked` if `agreed` is true, and omits it if false or null.
- The null-omits rule also handles optional attributes, e.g. `class="{{highlight}}"`.

Result: nothing a user types can turn into markup, so XSS is impossible by default.

#### Static includes: `{{> name}}`

```html
<body>
  {{> header}}
  ...
```

`{{> header}}` pastes in `frags/header.html` **at compile time**: no runtime cost, and no code needed, so the pure-HTML starting point still works with a shared header and footer.

- Fields inside an included fragment are filled from the same map as the page.
- Includes can nest; an include cycle is a compile error.
- **This is v1's only layout mechanism.** Each page remains a complete `<html>` document that includes its shared parts. Full layout inheritance (a page filling slots in a master layout) is left out of v1.

#### Missing and extra fields (forgiving)

- A field the template uses but `render` didn't provide renders as empty (or an omitted attribute), with a **dev-mode warning** in the request inspector and error overlay, since it's usually a typo.
- Keys returned by `render` that the template doesn't use are ignored, also with a dev-mode warning.
- A page with fields but no `render` is fine; its fields render empty. This is forgiving on purpose, because pages are often designed before their code exists.

### Handler model

A page's `.ms` file defines functions; it doesn't do its work at the top level.

**`render`** is a global function that fills the page's fields. It's optional: a page with no `.ms` file, or no `render`, is served as is.

```
render = function(req)
    return {"visits": data.increment("visits")}
end function
```

It returns a map of field values. (Rejected alternative: a bare top-level script whose globals fill fields of the same name. It's slightly simpler, but any global, including intrinsics like `time` or `len`, could collide with a field name, and there'd be no natural place to put other handlers.)

**Other handlers** are functions in maps named after HTTP methods, which the engine creates before the file runs:

```
get.more = function(req) ... end function     // GET  /guestbook/more
post.sign = function(req) ... end function    // POST /guestbook/sign
```

- `post.sign` in `pages/guestbook.ms` handles `POST /guestbook/sign`. Handler names can be any valid identifier.
- **The names mirror the markup:** `hx-post="/guestbook/sign"` ↔ `post.sign`, `hx-get="/guestbook/more"` ↔ `get.more`.
- **Rule of thumb:** use `get` for anything that only shows information, and `post` for anything that changes something. Every method other than GET is CSRF-checked (see §5), except `webhook` handlers (see §6).
- One URL can serve several methods: `get.entries` and `post.entries` both live at `/guestbook/entries`. `put`, `delete`, and `patch` maps can be added later in the same way. (MS2 has no global intrinsics with these names; `get` and `delete` exist only as members of the `key` and `file` modules.)
- **Only functions in these maps can be reached from a URL**, so helper functions in the same file are never exposed. Endpoints are easy to list: grep for `get.` and `post.`.
- `render` is deliberately global: it's the page's own GET, a fixed name the engine calls, not a user-chosen endpoint. Keeping it out of `get` means there's no separate `/guestbook/render` URL.
- A page and a `get` handler with the same URL (e.g. `pages/guestbook/more.html` and `get.more` in `pages/guestbook.ms`) is a compile error.
- (Considered: `actions` and `views` maps. The method names were preferred because they map directly onto HTML and HTTP, and teach the real GET/POST distinction.)

#### The request object

`req` is a **frozen** map (an MS2 frozen value): handlers can read it but never modify it.

| Field | Contents |
|---|---|
| `method`, `path` | `"GET"`, `"/guestbook"` |
| `query` | map of URL query parameters |
| `form` | map of submitted form fields: declared fields only, normalized and typed, if the handler has a `form` declaration (see [Forms and validation](#forms-and-validation)) |
| `errors` | `null`, or a map of validation errors by field name |
| `params` | values captured from route patterns (`{id}`) |
| `files` | uploaded files (contents held by the engine, saved to `data/` explicitly; type-checked by content when declared) |
| `headers` | read-only request headers |
| `user` | the signed-in user, or `null` (see §5) |
| `ip` | the client's address (from trusted proxy headers when behind a proxy; see §8) |
| `htmx` | true if HTMX made the request, so it expects a fragment |
| `rawBody` | the unparsed request body (`webhook` handlers only, for signature checks) |

`session` and `data` are globals, not fields of `req`. Raw cookies are left out for now, since `session` covers what beginners use cookies for.

#### Return values

| Handler returns | Response |
|---|---|
| a map (from `render` only) | the page, with its fields filled in |
| `reshow(frag, errors)` | the form again, with errors and the visitor's input (see [Forms and validation](#forms-and-validation)) |
| `frag(name, value)` | an HTML fragment |
| `redirect(url)` | a 303 redirect (or the HTMX equivalent if `req.htmx`) |
| `json(value)` | JSON, for API-style endpoints |
| `status(code, body)` | any status code, e.g. `status(404, frag("notFound"))` |
| `null` | 204 No Content |
| an `error` value | 500, logged (see Errors below) |

- **Rendered HTML is an unforgeable type, not a string.** `frag` returns an MS2 **handle**, an opaque, engine-created value, not a map (a map could be faked with `{"__isa": SafeHTML, "html": "<script>…"}`; a handle can't be built or forged by script code). A plain string returned from a handler is sent as `text/plain`, never as HTML. A string put into a field is always escaped. The only ways to produce HTML are templates and the explicit `unsafeHTML`. This enforces the template-safety rule at the API level.
- `frag` given a **list** renders the fragment once per item, so filling a list field needs no loop.

#### Errors

Errors, runtime failures, and budget overruns are all handled the same way:
- **Dev:** the in-browser overlay with file, line, and code.
- **Production:** a generic 500 page (customizable as `pages/500.html`). Details go to the engine log (`private/state/logs/`), never to the browser.
- Missing pages get `pages/404.html` if it exists.

#### Shared code

`import` resolves only within `code/lib/`: the "code only comes from the read-only partition" rule, applied to modules.

#### Progressive enhancement

Forms should carry both a normal `action` and `hx-post`. With JavaScript, HTMX swaps the result in place. Without it, the form posts normally and the handler redirects back (redirect-after-POST, which also stops a reload from resubmitting). It's the same handler either way, distinguished by `req.htmx`. See the guestbook example in §7.

### Forms and validation

Everything in a submitted form is untrusted, and handling it well takes more than checking it: values need parsing and normalizing, unexpected fields need dropping, and a form with mistakes has to come back **with the errors and with what the visitor typed**, or they'll give up. Doing that by hand in every handler is tedious, so it gets done inconsistently. Instead, a handler **declares the fields it accepts**, and the engine does the rest before the handler runs.

#### Declaring a form

A `form` map, alongside `get` and `post`, describes the fields each handler accepts, keyed by handler name:

```
// code/pages/guestbook.ms
form.sign = {
    "who":   {"type": "text", "required": true, "max": 40},
    "msg":   {"type": "multiline", "required": true, "max": 2000, "maxLinks": 2},
    "mood":  {"type": "choice", "options": ["happy", "meh", "grumpy"]},
    "photo": {"type": "file", "maxKB": 2048, "types": ["image/png", "image/jpeg"]}}
```

Like `access =`, `form` declarations are plain data, **read at compile time**. That lets the engine check them against the templates (below).

| Type | Accepts | Becomes |
|---|---|---|
| `text` | one line; newlines rejected | string |
| `multiline` | any text (line endings normalized to `\n`) | string |
| `email` | a syntactically valid address | string |
| `url` | http(s) only, the same rule as template URLs | string |
| `number`, `int` | strict decimal; no `Infinity`, `NaN`, or `1e400` | number |
| `bool` | a checkbox: present means true, absent means false | 1 or 0 |
| `choice` | one of `options` (a select or radio group) | string |
| `choices` | any subset of `options` (several checkboxes) | list of strings |
| `date` | `YYYY-MM-DD` (what `<input type="date">` sends) | string |
| `file` | an upload within `maxKB`, whose **content** matches `types` | upload handle (in `req.files`) |

Rules: `required`, `min`/`max` (length for text, value for numbers), `maxLinks` (counts URLs in text, a cheap spam filter), and `messages` to replace the default error messages per rule (e.g. `"messages": {"required": "Please sign your name"}`).

**What the engine does before the handler runs:**

- **Normalizes text:** trims surrounding whitespace, applies Unicode NFC, and strips control characters (except newlines and tabs in `multiline`). A value that's empty after trimming counts as missing, so `required` means something.
- **Converts types** as in the table, so `req.form.age` is a number, not a string.
- **Drops undeclared fields.** A tampered form that adds `role=admin` or `price=0` never reaches the handler (no mass assignment). The same goes for a `choice` value that isn't in `options`, which a visitor can send by editing the page.
- **Checks uploads by content**, not by the type the browser claims: a "PNG" must start like a PNG.
- **Validates**, and puts the results in **`req.errors`**: `null` if everything passed, otherwise a map from field name to message. The handler still runs either way, since only it knows what to show next.

A handler with no `form` declaration still gets `req.form`, as raw strings, still bounded by the engine's size limits (§8). The dev-mode access report flags `post` handlers that read `req.form` without a declaration.

#### Showing errors and keeping input

The form itself is a fragment, so it can be rendered both in the page and on its own:

```html
<!-- code/frags/signForm.html -->
<form id="signForm" method="post" action="/guestbook/sign"
      hx-post="/guestbook/sign" hx-target="#entries" hx-swap="beforeend">
  <p class="error">{{errors.form}}</p>
  <input name="who" value="{{who}}"> <span class="error">{{errors.who}}</span>
  <textarea name="msg">{{msg}}</textarea> <span class="error">{{errors.msg}}</span>
  <button>Sign</button>
</form>
```

```
render = function(req)
    return {"entries": frag("entry", data.get("guestbook", [])),
            "signForm": frag("signForm", formState("sign"))}
end function

post.sign = function(req)
    if req.errors then return reshow("signForm")
    if isBanned(req.form.who) then return reshow("signForm", {"who": "Sorry, not allowed."})
    entry = {"who": req.form.who, "msg": req.form.msg}
    data.append "guestbook", entry
    if req.htmx then return frag("entry", entry)
    return redirect("/guestbook")
end function
```

- **`formState(name)`** returns the map a form fragment needs: the submitted values (by field name) and an `errors` map (with `errors.form` for errors that aren't about one field). Normally it's empty. Right after a failed submission, it holds what the visitor typed and what went wrong. A field named `errors` is therefore a compile error.
- **`reshow(fragName, extraErrors)`** sends the form back with its errors, whichever way it was submitted:
  - **With HTMX:** it renders the fragment and tells HTMX (with `HX-Retarget` and `HX-Reswap` headers) to replace the form itself, rather than appending to `#entries`. That's why the form's root element needs an `id`. The status is 422; the engine's HTMX configuration swaps 422 responses.
  - **Without JavaScript:** it stores the values and errors for one use in the engine's part of the session (like a flash message, §2, but out of the app's reach) and redirects to the handler's own page (`/guestbook`, known from routing; never the `Referer`, so it can't become an open redirect). That page's `render` calls `formState("sign")`, which picks them up. Redirect-after-POST stays intact.
- `extraErrors` covers checks only the handler can do ("that name is taken"); they're merged with the declared rules' errors.
- Values are shown back through ordinary fields, so they're escaped like any other value.

#### Checking forms against templates

Because the engine parses templates at compile time, and `form` declarations are read at compile time, it can match each `<form>` to its handler by the `action`/`hx-post` URL and:

- **Warn about mismatches** in dev mode: an input whose `name` isn't declared (usually a typo), or a declared field with no input.
- **Add browser-side validation attributes** that match the declaration (`required`, `maxlength`, `min`, `max`) where the markup doesn't already have them, so visitors get instant feedback. The server still enforces everything; these attributes are only a convenience.

#### Still open

- Whether a `form` declaration should be **required** for any `post` handler that reads `req.form`, rather than just flagged.
- A `pattern` rule needs regular expressions, which depend on what MS2 provides; and so does localization of the default messages (see Open questions).
- Forms whose `options` are filled in by code can't be checked against the template at compile time.

### Markdown

GitHub-flavored Markdown (GFM) is a first-class format alongside HTML: for whole pages, for fragments, and as a value that code can render. It has become a quasi-standard (READMEs, dev.to, Discord, LLM output), it's easy to write, and it's readable even as source.

#### Fourfold Markdown: GFM, made safe

Plain GFM is **not** safe: it passes raw HTML through, and GitHub is safe only because it sanitizes the output afterward. Fourfold Markdown is GFM with these changes, applied by the engine's renderer everywhere:

- **No raw HTML.** HTML tags in Markdown are shown as escaped text, in developer files too. That's one rule and one renderer configuration; anyone who wants HTML writes an HTML fragment.
- **Link and image URLs follow the template URL rule** (see [contextual escaping](#where-fields-are-allowed-contextual-escaping)): http(s), mailto, or relative only. `[click](javascript:…)` renders as plain text.
- **Linear-time parser, capped input.** Rendering runs in native code, outside the VM's instruction budget, so the parser must not slow down on pathological input (nested brackets, emphasis runs). md4c and cmark-gfm are both designed for this, and md4c has a built-in no-HTML flag. Text rendered at runtime is also capped in size.
- Task-list checkboxes are rendered `disabled`.

Markdown with no raw HTML can't execute anything. Markdown that reaches `markdown()` at runtime, or is kept in `data/`, has no fields or includes either, so it is **inert data** under the core rule. It can be rendered from anywhere, including `data/`: a wiki or blog stored as data can be shown with `markdown(file.readLines(path), "full")` (`markdown` takes a list of lines as well as a string; M5.1). Markdown files in `code/` can have fields and includes (see [Fields in Markdown](#fields-in-markdown)), which makes them templates, like HTML files in `code/`.

#### Markdown pages

- `pages/about.md` is served at `/about`, rendered as HTML, and at `/about.md` as Markdown (see [Markdown as Markdown](#markdown-as-markdown)). This is an exception to the static-file rule: `.md` files in `pages/` are pages, not static files.
- A page must be a complete document, so the engine wraps the rendered Markdown in **`frags/layout/markdown.html`**, which has a `{{content}}` field and a `{{title}}` field (from the first `#` heading). If that template doesn't exist, a minimal built-in page is used, so a pure-Markdown site needs no setup at all.
- This is one narrow wrapper, not general layout inheritance, which is still out of v1. Per-folder wrappers (the nearest one up the tree wins) could come later.
- `about.md` and `about.html` side by side is a compile error, like any other route collision.
- `about.md` pairs with `about.ms`, exactly as an HTML page does: `render` fills its fields, and `get` and `post` work the same (M6.1).
- No front matter in v1.
- **Links between `.md` files are rewritten at compile time** in the HTML output: `[setup](setup.md)` becomes `/setup`, keeping any `#anchor`. The same folder reads correctly on GitHub or in an editor's preview, and when served by Fourfold. Since the route table is known at compile time, **a link to a route that doesn't exist is a compile warning.**
- Headings get GitHub-style anchor `id`s, so `#installation` links work.

#### Markdown fragments

- `frags/intro.md` works with both `frag("intro")` and `{{> intro}}`. As an include, it's rendered at compile time, so it costs nothing per request.
- `.md` fragments have fields, like HTML fragments, and `frag("row", list)` renders one per item (M6.1).
- `intro.md` and `intro.html` side by side in `frags/` is a compile error.

#### Rendering Markdown from code

`markdown(text)` renders a string and returns **the same rendered-HTML handle as `frag`**. Nothing about templates changes: in text context the result is inserted as HTML; in an attribute it's an error.

```
showEntry = function(e)
    return frag("entry", {"who": e.who, "msg": markdown(e.msg)})
end function
```

Store the Markdown source, not the rendered result. Handles aren't inert, so they can't be saved to `data` or `session`, and rendering at display time lets the renderer's rules improve without migrating stored data.

#### Two profiles

Text that reaches `markdown()` at runtime is usually visitor input, so **the default is the restricted `"comment"` profile**. Full Markdown is an explicit, grep-able opt-in: `markdown(text, "full")`. Files in `code/` are developer-authored and always use `"full"`.

| | `"comment"` (default for `markdown()`) | `"full"` (`.md` files; opt-in from code) |
|---|---|---|
| Paragraphs, line breaks | yes | yes |
| Bold, italic, strikethrough, inline code | yes | yes |
| Code blocks, lists, block quotes | yes | yes |
| Tables | yes | yes |
| Links | yes, with `rel="nofollow ugc"` | yes |
| Headings | rendered as ordinary paragraphs | yes |
| Images | rendered as a plain link to the image URL | yes |
| Footnotes | no | yes (not yet: md4c has no footnotes; deferred in M4) |
| Heading and footnote `id`s | **none** | yes |

- **Why no images in comments:** a visitor's image is a tracking pixel or a hotlink to someone else's server.
- **Why no `id`s in comments:** user content that produces an `id` can **clobber** the page's own. A comment containing `# entries` would produce `id="entries"` and break the guestbook's `hx-target="#entries"`. (GitHub works around this by prefixing `user-content-`. Leaving `id`s out is simpler.)
- **Why headings become paragraphs:** a visitor's `# Hello` shouldn't render as the biggest text on the page.

#### Styling

Styling rendered Markdown should take no effort:

- All rendered Markdown is wrapped in an element with a class, `<div class="markdown">` (with `markdown comment` for the comment profile), so CSS can target `.markdown h2`, `.markdown table`, or `.markdown.comment` without affecting the rest of the page.
- **Stylesheet by convention:** if `pages/markdown.css` exists, the engine adds a `<link>` to it in the `<head>` of every page that contains rendered Markdown, the same way it injects CSRF tokens. If it doesn't exist, the engine links its own built-in default stylesheet (at a reserved URL such as `/__fourfold/markdown.css`), so Markdown looks decent out of the box. A config setting turns injection off for developers who'd rather link their styles themselves.
- Code blocks render as `<pre><code class="language-x">`, so any CSS/JS syntax highlighter works. (Server-side highlighting could come later.)
- Live reload already swaps changed CSS in place (see §9), so tuning Markdown styles is immediate.

#### Fields in Markdown

*Decided 2026-10-28; built in M6.1 ([milestones.md](milestones.md)).* Many sites could be written entirely in Markdown. If a page needs HTML the moment it shows one dynamic value, the developer is pushed back to HTML for no good reason, and a Markdown-only client (such as a Markdown browser in Mini Micro) can't see that page at all. So Markdown pages and fragments can do what HTML ones do:

- **Fields only in developer Markdown in `code/`**, never in text passed to `markdown()` at runtime. Runtime and `data/` Markdown stays inert.
- **The same syntax and rules as HTML templates.** `{{name}}` is a field, `{{user.name}}` a lookup, `{{> intro}}` an include, and `\{{` a literal `{{`. A page with fields but no code renders them empty, and dev mode warns about missing and extra fields, as for HTML.
- **Never recognized in code spans or code blocks**, so documentation examples like `` `{{name}}` `` work as they are. (Every `{{` in the sample apps' Markdown today is in a code span.)
- **Parse first, fill second.** The compiler parses the Markdown with each field as an opaque placeholder, and records where each one landed. A field value is placed after parsing, so it can never change the Markdown structure: a value containing `**`, `# ` or a newline stays literal text.
- **One parse, two outputs.** Each `.md` template compiles to an HTML form (for `/about`) and a Markdown form (for `/about.md`), with the same fields in the same places. Each output escapes values for its own format:

| Where the field is | HTML output | Markdown output |
|---|---|---|
| Text (paragraphs, headings, emphasis, list items, table cells, link text, image alt text) | escaped as HTML text | every ASCII punctuation character backslash-escaped (CommonMark allows this for all of them, so `*`, `_`, `#`, `[`, `<`, `&` and the rest stay literal, and `\|` keeps a table cell whole); CR, LF and tab become spaces; leading spaces at the start of a line written as `&#32;`, so they can't start a code block |
| Start of a link or image destination: `[profile]({{url}})` | the template URL rule (http, https, mailto, or relative; anything else becomes a dead link) | the same URL rule, then spaces, `( ) < >` and control characters percent-encoded |
| Later in a destination: `[next](list.md?page={{n}})` | percent-encoded | percent-encoded |

- **Where fields can't go** (compile errors, with a message saying what to do instead): link reference definitions and labels (`[x]: …`, `[text][{{ref}}]`), autolinks (`<{{url}}>`), and fenced-code info strings. A heading's `id` comes from its text with the fields left out, so it doesn't change with the values.
- **Values:** a string is escaped, a number is written as MiniScript prints it, null is empty, and a map or list is an error, as in HTML text.
- **Includes and rendered values are blocks.** `{{> name}}` and a field holding a rendered value (from `frag` or `markdown`) must stand alone on a line, at the top level (not inside a list item or block quote), and stand for whole blocks: never wrapped in a `<p>`. A Markdown template includes and takes only **Markdown**: an `.md` fragment, or a `markdown()` result. An HTML fragment or `unsafeHTML` in a Markdown template is an error (at compile time for an include, at render time for a value), in both outputs, so `/about` and `/about.md` never disagree about what works.
- **Rendered values are rendered late.** `frag` and `markdown` return a handle that records what to render (the fragment and its values, or the text and its profile), and the handle is rendered in the format of the template it lands in. An `.md` fragment renders as either; an HTML fragment only as HTML. In the Markdown output, `markdown(text)` is parsed and written back out as Markdown, applying its profile (comment headings become paragraphs and images become links; raw HTML stays escaped text), so visitor text reaches a Markdown client under the same rules as a browser. Handles are still not inert, and still can't be saved.

#### Markdown as Markdown

*Decided 2026-10-28; built in M6.1.* A client that reads Markdown rather than HTML (a Markdown browser in Mini Micro, an LLM or agent, a command-line tool) can fetch any Markdown page as Markdown.

- **By extension, not by `Accept` header:** `/about` is HTML and `/about.md` is `text/markdown; charset=utf-8; variant=GFM`. The `.md` links in the source already point at these URLs (`setup.md` from `/docs/about.md` resolves to `/docs/setup.md`), so the Markdown output keeps links as written; an index page is `/docs/` as HTML and `/docs/index.md` as Markdown. One URL per format keeps caching simple, with no `Vary: Accept`. Negotiating on `Accept: text/markdown` could be added later.
- **On by default.** The Markdown shows nothing the HTML page doesn't. (This replaces an earlier plan for an opt-in setting serving the raw source.)
- **One route, two formats.** `/about.md` is the same `RouteEntry` as `/about`: the same access rules, the same gate, the same code. `req.format` is `"html"` or `"markdown"`, for the rare handler that cares. It exists only for Markdown pages: `/contact.md` for an HTML page is a 404.
- **Filled, not raw.** The response is the page's Markdown form with its fields filled and its includes in place, never the source with `{{visits}}` in it. A page with no fields is sent as written, and is the same on every request.
- **No layout.** The Markdown output is the page alone; `frags/layout/markdown.html` wraps only the HTML. The HTML output gets `<link rel="alternate" type="text/markdown" href="/about.md">` in its `<head>`, so clients can find the Markdown.
- **Raw HTML in the source** is sent as written, since it's the developer's own file. Fourfold's renderer shows it as text, and a client that follows Fourfold Markdown (as Mini Micro's will) does the same; a generic GFM client might render it.
- **Reading only.** Markdown has no forms, so a Markdown client can follow links (including ones with query strings, `list.md?page=2`) but not post. A restricted page needs a session cookie, and signing in is an HTML flow; Markdown clients that sign in belong to [non-browser clients](#open-questions).

---

## 5. Authentication and access control

### Threat model

**Code in the read-only partition is trusted; everything arriving over the network is not.** We're defending against visitors (including malicious ones) and against the developer's own mistakes, not against the developer. So it's fine for app code to do privileged things like granting roles.

### Built into the engine

App code never sees a password, password hash, login token, or session cookie. The engine owns:

- the sign-in, sign-out, sign-up, and recovery flows, at reserved URLs (e.g. `/__auth/login`);
- credential storage, in an **engine-owned store in `private/state/`**, outside the app's `data/` partition, so even a badly buggy app can't leak credentials;
- rate limiting, lockouts, email verification, and session handling.

The app sees only `req.user`.

### Sign-in methods

**v1:** email **magic link**, plus the **dev-mode fake login**.

- Magic links require email sending to be set up in the host config (SMTP or a provider).
- *As built in M6:* the emailed link opens a page with a "Sign in" button, and the POST from it signs in, so mail scanners that fetch links can't use the token. A link works once, for 15 minutes; links are throttled per address and per visitor. The form answers "check your email" whether or not the address may sign in. A first sign-in creates the user unless `auth.signUp` is `"closed"`. See [m6-results.md](m6-results.md).
- **Dev-mode fake login:** a "sign in as…" picker with a few test users, including an admin. No email or setup needed, so testing "what does an admin see?" takes one click.

**Later**, enabled per app in the host config:

| Method | Pros | Cons |
|---|---|---|
| Passkeys (WebAuthn) | Phishing-proof; no secrets stored | Still unfamiliar to some users; needs a fallback |
| OAuth ("Sign in with Google/GitHub") | Familiar; no email needed | Developer must register the app with each provider |
| Password | Everyone expects it | Weakest; the engine handles argon2id, breach checks, rate limits |

### Login pages fit the app's design

The built-in login pages are plain. To match the site, the developer can supply templates like `frags/auth/login.html` that wrap the login UI. **The engine still renders the form itself** into a field of that template, so a designer controls the look but can't accidentally weaken the form (drop the CSRF token, post somewhere else, etc.).

### The user object

`req.user` is `null` or a frozen map:

| Field | Contents |
|---|---|
| `id` | stable, opaque string; use it as the key for app data |
| `name` | display name |
| `email` | verified email address |
| `roles` | list of strings, e.g. `["admin"]` |

- App-specific profile data (avatar, preferences, score) is ordinary app data, stored with `data` and keyed by `user.id`. The auth store holds only identity.
- **Roles:** the host config names the initial admins (by email). App code can grant or revoke roles through a small `users` module, e.g. `users.grant id, "moderator"`. Per the threat model, that's fine: only trusted code can call it.

### Access control

Access is declared, and checked by the engine **before** any handler runs. Pages are **public by default**. A rule's level is `"login"` (any signed-in user) or a role name such as `"admin"`.

**1. In a page's `.ms` file:**

```
access = "login"
```

This applies to the page (`render`) and to every `get`/`post` handler in that file.

**2. In the central access file, `code/access.ms`:**

```
rules = {}
rules["/admin"]           = "admin"
rules["/members"]         = "login"
rules["/guestbook/sign"]  = "login"
rules["/forum/mod"]       = ["admin", "moderator"]
```

The central file covers what `.ms` files can't: whole folders, plain HTML pages with no code, individual handlers (e.g. a public page whose `post.sign` requires sign-in), and **static files of any type**. For example, images or videos under `pages/members/` are restricted along with the pages there. Restricted responses are sent with `Cache-Control: private, no-store`, so shared caches and CDNs never serve them to anyone else.

There's no per-handler form of `access =` in `.ms` files; exceptions within a file go in the central file.

**Paths: no wildcards.** A rule applies to its path **and everything below it**, matched on whole path segments: `"/admin"` covers `/admin`, `/admin/users`, and `/admin/media/logo.png`, but not `/administrator`. There are no mid-path wildcards (`/users/*/edit`) or extension rules (`*.mp4`); parameterized routes can use `access =` in their handler's file.

**Values:**

| Value | Meaning |
|---|---|
| `"login"` | any signed-in user |
| `"admin"` (any role name) | signed in with that role |
| `["admin", "moderator"]` | signed in with **any** of these roles |

**Evaluated at compile time, as data.** The engine runs `access.ms` at compile time in a restricted VM with no `data`, `session`, or I/O. Its only output is the `rules` map of strings and lists. The syntax is MiniScript, but in practice it's a data file. Changes take effect on recompile, so access policy is deployed the same way as code.

**How the rules combine, and protection against failing open:**

- **Every applicable rule must be satisfied** (AND across rules; a list within one rule is OR). If the central file says `"login"` and the page says `"admin"`, the visitor needs to be an admin. Adding a rule anywhere can only tighten access, never loosen it.
- **Consequently, there are no public exceptions inside a protected folder.** If `/admin/help` should be public, move it out of `/admin`.
- **A central rule that matches nothing is a compile warning.** This catches the common failure where a folder is renamed and its rule silently stops applying.
- **Dev-mode access report:** a table of every URL (pages, handlers, static files) with its effective access level and which rules produced it. Reviewing security means reading one table.

### Access is resolved per route, at compile time

The compiled app contains a **route table** of every URL the app can serve: pages, `get`/`post` handlers, routes-table entries (including parameterized ones like `/user/{id}`), and static files. The compiler calculates each route's effective access level once and stores it on the route entry. At request time there is no pattern matching:

1. The request path is canonicalized once: percent-decoded, repeated slashes collapsed, dot segments rejected. Matching is case-sensitive.
2. The route is looked up in the compiled table, **never on the filesystem**. A path not in the table is a 404.
3. The route's stored access level is checked before any handler runs.

Because the access check and the content lookup use the same table entry, the usual normalization bypasses can't happen. Examples are `/Admin` versus `/admin` on a case-insensitive filesystem, `%2e%2e`, `//admin`, and Unicode lookalikes. The access report is simply a dump of this table, and the "rule matches nothing" warning comes almost free: it's any rule that applied to no route.

**When access is denied:**
- A GET from a visitor who isn't signed in redirects to sign-in, then back to the original page.
- Any other request gets 401 (not signed in) or 403 (signed in, but lacking the role).

Finer-grained checks ("only the author can delete this entry") stay in handler code, using `req.user`.

**Rejected:** a `<meta name="access">` tag in a page's `<head>`. It would put a security setting in the designer's file, and a redesign that replaces the `<head>` would silently make the page public. Security decisions stay out of HTML, just as code does. Also rejected: per-folder `_folder.ms` files, which the central file replaces.

### Sessions and cookies

(Session storage, timeouts, and size limits are covered in §2, [Session storage](#session-storage).)

- Cookies are `HttpOnly`, `Secure`, `SameSite=Lax`, with the `__Host-` prefix.
- **The session ID rotates at sign-in**, preventing session fixation. Whatever was in the anonymous `session` (a shopping cart, say) carries over.
- **Signing out deletes the session on the server**, not just the cookie.
- **Anonymous sessions are created only when code first writes to `session`.** Bots and crawlers that never touch it cost nothing.

### CSRF protection

Because the engine renders every page, it can protect forms automatically:

- It adds a hidden token field to every `<form method="post">` when rendering, and an `hx-headers` token on `<body>` for HTMX requests.
- The token is a keyed hash of a **CSRF cookie of its own**, not part of the session, so a visitor who never writes to `session` still gets no session record (decided in M5).
- **Any request other than GET without a valid token is rejected before the handler runs.** The `Origin` header is checked as well.
- The developer never has to think about any of this.

GET requests are not token-checked, which is why state-changing handlers belong in `post`, not `get`. This is a documented rule, not enforced: making `data` read-only during GETs would break harmless side effects like the visitor counter in `render`.

The one exception to token checking is `webhook` handlers, which receive POSTs from outside services (see §6).

### Rate limits and spam

Spam is the first problem almost every public form meets. The defenses are layered, from weakest to strongest. The defaults need no work from the developer; the stronger defenses are one line each.

| Defense | Default | Stops |
|---|---|---|
| Rate limits | on, for every non-GET request | floods; a bot posting as fast as it can |
| Content rules (`maxLinks`, nofollow links) | per form | link spam, and much of the motive for it |
| Timing check | opt-in per route | bots that submit the moment they fetch a form |
| Proof of work | opt-in per route | cheap bulk posting |
| Requiring sign-in | opt-in per route | nearly everything |

None of the middle three defeats a determined, targeted spammer. They raise the cost of each post above zero, and they work from **state on the server** (signed tokens, counters) that a bot can't see or fake.

#### Rate limits

**Default limits on everything but GET.** Every non-GET request is limited per client (e.g. 60 per minute per IP for anonymous visitors, more for signed-in users), so an unprotected form can't be flooded. Exceeding a limit gets a 429 with `Retry-After`, using `pages/429.html` if it exists.

**Per-route rules** go in `code/limits.ms`, evaluated at compile time as data, like `access.ms`, with the same path rules (a rule covers its path and everything below it):

```
limits = {}
limits["/guestbook/sign"] = ["5/minute", "50/day"]
limits["/api"]            = "100/minute per user"
limits["/forum/post"]     = {"rate": "10/hour", "minSeconds": 3, "challenge": true}
```

A string or list is shorthand for rates only; a map can also turn on the timing check (`minSeconds`) and proof of work (`challenge`), both below.

- **Rate syntax:** `count/unit` with `second`, `minute`, `hour`, or `day`, optionally followed by `per ip`, `per user`, or `per session`. `per ip` is the default. `per user` falls back to per-IP for visitors who aren't signed in.  ` per ` and `/` are synonyms, so "100/minute/user", "100 per minute per user", and "100/minute per user" are all equivalent.
- **Every applicable rule applies**, as with access rules: the defaults, any folder rule, and the route's own rule. Adding a rule can only tighten.
- **IPv6 addresses are limited by /64 prefix**, not by full address, since anyone with one IPv6 address usually has a whole /64.
- **Shared IPs are a real concern for this audience.** A classroom or school behind one IP address looks like one very busy visitor. That's why the defaults are generous, why signed-in users are counted per user rather than per IP, and why the dev-mode report shows each route's effective limits.
- **Limits from code**, for keys only the app knows: `ratelimit("comment:" + postId, "20/hour")` returns true if the request is within the limit (and counts it), false if not.
- Counters are kept in `private/state/` (cached in memory), so they work in CGI mode and survive restarts.

The engine's own sign-in flows have their own fixed limits, including **per email address**, so nobody can use the magic-link form to flood someone else's inbox.

#### Content rules

- `maxLinks` in a form declaration (see §4, [Forms and validation](#forms-and-validation)) rejects link-stuffed messages, the commonest spam.
- Visitor-supplied Markdown links are already `rel="nofollow ugc"` (§4, [Markdown](#markdown)), which removes much of the SEO motive.

#### Timing check: `minSeconds`

The engine already adds a token to every `<form method="post">` it renders (for CSRF). On routes with `minSeconds`, that token also carries **the time the form was rendered**, signed with a key only the engine has.

- A submission that arrives sooner than `minSeconds` after the form was rendered is rejected before the handler runs, with a friendly "that was quick; please try again" response, since now and then it's a fast human. A bot can't forge an earlier time; it has to fetch the form and actually wait, which costs it time on every post.
- It's opt-in, and best for forms where people type something. Fast humans are real: a one-button "like" form would trip it.
- A request to the route without a valid timing token (e.g. a bot posting directly, without fetching the form) is rejected.
- **Old forms aren't rejected by default,** because people leave tabs open overnight. A maximum age is an option.

#### Proof of work: `challenge`

On routes with `challenge`, the form carries a signed, single-use puzzle: find a number that, hashed with the server's random challenge, gives a result with a certain number of leading zero bits. The engine's JavaScript (served with HTMX) starts solving it when the visitor starts filling in the form, so it's normally done before they click Submit. The server checks the answer with a single hash.

- **Invisible to people:** no images to decipher, nothing to click, no third party, no tracking. The difficulty is set so that solving takes a fraction of a second on a phone.
- **A real cost for bulk posting:** each submission needs its own solution, and each challenge can be used once (the engine remembers used ones until they expire).
- **It needs JavaScript**, which breaks the no-JS fallback (§4, [Progressive enhancement](#progressive-enhancement)) for that form. Visitors without JS see a message saying the form needs it. That's the main reason it's opt-in.
- A spammer with plenty of computing power can pay the cost, so this reduces bulk spam rather than ending it.

#### The strongest defense: signing in

Requiring sign-in to post, which is one line in `access.ms` (`rules["/guestbook/sign"] = "login"`), stops nearly all spam, because every account needs a working email address, and limits can then be per user instead of per IP. The docs should present this as the first choice for anything beyond a toy.

#### Considered and rejected: an automatic honeypot field

A hidden field that people can't see but form-filling bots fill in. It catches only the most generic bots, and as a framework feature it's worse than useless:

- **The same honeypot on every Fourfold site gets beaten on every Fourfold site.** It's worth a bot author's time to add one rule that skips it.
- **Its giveaway markup is required for accessibility:** without `aria-hidden`, screen-reader users would hear a strange "leave this empty" field.
- **Real people trip it:** browsers' autofill often ignores `autocomplete="off"`, and password managers fill hidden fields, so honest visitors get rejected with no visible reason.

#### Dev mode

- The access report shows each route's effective rate limits, timing check, and challenge next to its access level.
- The request inspector shows which limits a request counted against and their current counts, and why a submission was rejected (too fast, missing token, bad challenge). The dev tools can reset counters.
- Challenge difficulty is minimal in dev, so testing isn't slowed down.

#### Still open

- Whether timing tokens should be single-use. As designed, reuse of one token is bounded only by the rate limits.
- Integration with a third-party challenge such as Cloudflare Turnstile, for apps that want it. It needs outbound access and CSP additions.
- How the challenge difficulty should be set: fixed per route, or raised automatically when a route sees a burst of posts.

---

## 6. Outbound HTTP, secrets, and webhooks

### Threats

Under the threat model, app code is trusted; the danger is **network input steering an outbound request** (a visitor-supplied URL, hostname, or path):

- **SSRF:** "fetch a preview of this link" pointed at `http://169.254.169.254/` (cloud credentials), `localhost:5432` (the database), or the engine's own internal endpoints.
- **DNS rebinding:** a hostname that resolves to a public IP when checked and a private one when connected.
- **Redirects** from an allowed host to a forbidden one.
- **Secret leakage:** an API key sent to the wrong host, or ending up in a page or a log.
- **Resource abuse:** slow or huge responses, decompression bombs, the app being used to flood someone else.

Use cases to support: calling known APIs (weather, maps, payments, Discord/Slack webhooks), which is by far the most common; and fetching visitor-supplied URLs (link previews, RSS readers), which is rarer and riskier, but legitimate.

### API

A host-provided `http` module, familiar from Mini Micro:

```
resp = http.get("https://api.weather.gov/points/" + lat + "," + lon)
if resp isa error then return status(502, frag("weatherDown"))
forecast = resp.json
```

- Calls are **synchronous**, which is fine with per-request VMs: the engine keeps serving other requests meanwhile. Waiting counts against the request's wall-clock budget, and each call also has its own timeout.
- The response is a map: `status`, `headers`, `body`, plus `json` parsed into inert values.
- Failures (timeouts, refused destinations, size overruns) return `error` values rather than halting.
- Responses are untrusted input. Putting one in a template is already safe, since fields are always escaped.

### Destination allowlist: `code/outbound.ms`

```
allow = ["api.weather.gov", "discord.com", "*.stripe.com"]
```

- Evaluated at compile time like `access.ms`, so it deploys and versions with the code.
- Hostnames only; HTTPS only by default; ports 443/80 only unless written as `"host:port"`.
- A leading `*.` matches subdomains. (Unlike access paths, hostnames do need this, since API providers spread across subdomains.)
- **No `outbound.ms` means no outbound HTTP.** The ability must be asked for explicitly.
- **`allow = ["*"]` permits any public host**, for link previews and RSS readers. It's allowed, but flagged prominently in the dev-mode outbound report. The always-on protections below still apply.
- **The host config can only tighten this**: disable outbound HTTP for an app, or cap its list, but never widen it. That's the same "rules can only tighten" principle as access control.

### Always-on network protections

These apply regardless of the allowlist, and apps can't change them:

- **Private and reserved IPs are blocked:** loopback, RFC 1918, link-local (cloud metadata), CGNAT, IPv6 ULA and link-local, IPv4-mapped IPv6, `0.0.0.0`, multicast. If a deployment truly needs an internal service, the host admin can allow that specific address in the host config.
- **Checked on the resolved IP, then pinned:** the engine resolves the hostname, checks the IP, and connects to that same IP (setting Host/SNI to the hostname). This defeats DNS rebinding.
- **The engine follows redirects itself**, rechecking every hop against both the allowlist and the IP rules; at most about 5 hops, and never downgrading HTTPS to HTTP.
- **Limits:** response size (also after decompression), connect and read timeouts, and an outbound request rate per app.

### Secrets: opaque handles bound to hosts

API keys live in `private/config/secrets.ms` (see §10), which MiniScript code can't reach. Each secret lists the host(s) it may be sent to, e.g. `weatherKey` → `api.weather.gov`; a secret with no hosts is engine-only.

App code sees each secret as an MS2 **handle**: an opaque, engine-created value that script code can't build, forge, or look inside. The real value is substituted only when the engine actually sends a request to one of that secret's allowed hosts:

```
resp = http.get(url, {"Authorization": ["Bearer ", secrets.weatherKey]})
```

- A header value (or URL) can be a string, a secret handle, or a **list of strings and handles** that the engine joins at send time. This covers "Bearer " prefixes and `?key=` query parameters without the app ever holding the secret as a string.
- A request carrying a secret to any host not tied to it returns an error and is not sent.
- Wherever a handle ends up (printed, concatenated, put in a template field, written to `data`, logged), only something generic like the type name appears, never the secret. Template fields and data serialization reject handles outright, since they're not inert values.

So a bug or crafted input can't leak a key into a page, a log, or a request to the wrong server. (Rendered fragments use the same handle mechanism; see §4.)

### Inbound webhooks: `webhook.x`

Payment providers, GitHub, etc. call **into** the app with POSTs that can't carry our CSRF token. These go in a separate handler map:

```
webhook.stripe = function(req)
    if not stripeSignatureOK(req.rawBody, req.headers, secrets.stripeWebhookKey) then return status(400, null)
    ...
end function
```

- `POST /<page>/stripe` calls `webhook.stripe` in that page's `.ms` file. It accepts POST only, **without** a CSRF token.
- In place of the token, the handler verifies the provider's signature using `req.rawBody` (provided only to webhook handlers).
- Being a separate map keeps CSRF exemptions explicit and easy to grep for, and the dev-mode reports list them.
- Access rules still apply to webhook URLs. In practice they're usually public, since the caller isn't a signed-in user.

### Visibility

- Every outbound request is logged to the engine log in `private/state/logs/` (host, status, duration, bytes), with secrets redacted.
- The dev-mode request inspector shows each outbound call made during a request.
- A dev-mode **outbound report**, alongside the access report, lists allowed hosts, which secrets are tied to which hosts, any `"*"`, and all `webhook` endpoints.

---

## 7. Partial page updates and server push

### Fragments with HTMX

Pages can update in place without a full reload, using the HTMX approach: HTML attributes declare what should happen, the server returns HTML **fragments**, and a small generic JS library swaps them into the page. The engine serves HTMX itself (one small JS file, about 14 KB, open source), so developers write no JavaScript.

```html
<!-- code/pages/guestbook.html (excerpt from inside <body>) -->
<ul id="entries">{{entries}}</ul>

<form method="post" action="/guestbook/sign"
      hx-post="/guestbook/sign" hx-target="#entries" hx-swap="beforeend">
  <input name="who"> <input name="msg"> <button>Sign</button>
</form>
```

```html
<!-- code/frags/entry.html -->
<li><b>{{who}}</b>: {{msg}}</li>
```

```
// code/pages/guestbook.ms
render = function(req)
    return {"entries": frag("entry", data.get("guestbook", []))}
end function

post.sign = function(req)
    entry = {"who": req.form.who, "msg": req.form.msg}
    data.append "guestbook", entry
    if req.htmx then return frag("entry", entry)    // just the new <li>
    return redirect("/guestbook")
end function
```

This fits the per-request model unchanged: request → handler → filled-in template → response. The only new idea is that the response can be a fragment from `frags/`, not a whole page.

### Real-time push via Server-Sent Events

1. A page subscribes to a channel, e.g. `<div hx-ext="sse" sse-connect="/live/guestbook">`.
2. The **engine** (not a MiniScript VM) holds the open connections: just a list of sockets per channel.
3. A handler calls something like `publish "guestbook", frag("entry", entry)`.
4. The engine writes that fragment to every subscriber, and HTMX swaps it in.

No VM lives beyond its request, so the core security property still holds. WebSockets could be added later if two-way connections are needed. SSE needs a long-running engine process, so it isn't available in CGI mode (see [Hosting modes](#hosting-modes)).

---

## 8. Deployment

- The developer uploads files with plain **scp/rsync**. No special tooling is required.
- **Whole-app compile and swap:** the engine compiles the entire code tree into a single compiled app, then swaps it in atomically. If anything fails to compile, it keeps serving the old version and logs the error, so a broken or partial upload never takes the site down. Requests already running finish on the old version; new requests get the new one. If the new version has pending migrations, the swap waits for them (see §3, [Schema changes and migrations](#schema-changes-and-migrations)).
- **Wait for quiet:** the engine watches `code/` and recompiles only once files have stopped changing for a second or two. Optionally, touching a trigger file (e.g. `code/.deploy`) means "load now."
- An optional `fourfold deploy` wrapper could rsync to a staging folder and then swap directories atomically.
- **Deploys should only touch `code/`.** The docs must warn that, for example, `rsync --delete` of the whole tree from a laptop would wipe production data.

### User uploads

Files uploaded into `data/` can never execute. But if the app serves them back, uploaded `.html`/`.svg` files could still cause stored XSS in the browser. The engine's file-serving function for `data/` should:
- set `Content-Type` from an allowlist,
- send `X-Content-Type-Options: nosniff`, and
- default to `Content-Disposition: attachment` for anything that isn't a known-safe type.

Static files in `code/pages/` are served normally, since only the developer can put them there.

### Hosting modes

Fourfold is its own web server, which in practice means a VPS, a container platform (Fly.io, Render, Railway, and the like), or a machine of your own. Shared hosting, where Apache is already running, mostly rules that out. Long-running processes are often forbidden or killed, binding a port is often blocked and ports 80/443 are never available, and `.htaccess` proxying needs `mod_proxy`, which is usually disabled. (Some cPanel hosts offer Phusion Passenger "apps", but typically only for Node, Python, and Ruby.) An Apache module wouldn't help either: shared hosts won't install custom modules, and anyone who could install one could just run Fourfold.

But nearly every shared host runs CGI, and many run FastCGI. The execution model (§2) is already close to CGI: a fresh VM per request, nothing kept in memory between requests, sessions in SQLite. So Fourfold supports two hosting modes:

| | **Standalone server** (primary) | **CGI / FastCGI** (reduced) |
|---|---|---|
| Typical host | VPS, container platform, own machine; `fourfold dev` | Shared hosting behind Apache |
| Startup per request | none (compiled app held in memory) | loads the compiled app from `private/state/` (CGI); none once warm (FastCGI) |
| Recompile on change | file watcher, wait for quiet | checked on request (`code/` changed, or `.deploy` touched) |
| SSE push (§7) | yes | **no**: fall back to HTMX polling (`hx-trigger="every 5s"`) |
| OS-level enforcement (§1) | separate engine user, read-only `code/` | **engine-only**: under suEXEC the engine runs as the developer |
| Budgets, outbound HTTP, email | Fourfold's config | Fourfold's config, but the host's own limits apply on top |

**Architecture.** The engine core is a library that doesn't care how a request arrives. The standalone HTTP server and a thin CGI/FastCGI adapter both sit on top of it. To keep the adapter thin, **all state that crosses requests lives in SQLite or files**, never only in process memory: sessions and their locks, rate-limit counters, the anonymous-session caps, and the atomic `data` operations must all work across separate processes. In-memory caches are fine as long as they're only caches.

**Apache must never serve the app's files.** The app folder (`code/`, `data/`, `private/`) lives **outside** the web root (`public_html`). Inside it there's only a tiny CGI entry point and an `.htaccess` that sends **every** request through it, static files included. If Apache served `pages/members/photo.jpg` itself, it would bypass access control; if it could reach `data/` or `private/`, far worse. A setup command (e.g. `fourfold cgi-setup`) writes this layout, and the engine **refuses to start if the app folder is inside the web root.**

**Visibility.** The dev-mode reports and the engine log state which mode the app is running in, and what that mode doesn't provide (e.g. "CGI mode: SSE push unavailable; OS-level partition enforcement unavailable").

The standalone server is what the docs recommend for real deployments, and a VPS costs about the same as shared hosting. CGI mode is there so someone with an existing shared-hosting account can try Fourfold without buying anything new.

### Serving on the open internet

In standalone mode, Fourfold is the server facing the internet, so it takes on what Apache or nginx would normally handle. The goal is the same as everywhere else: **safe defaults, with nothing for the developer to set up.**

#### TLS

Two supported shapes:

| | **Fourfold terminates TLS** | **Behind a reverse proxy** |
|---|---|---|
| Typical use | a single VPS | nginx/Caddy already running; cloud load balancers; container platforms |
| Certificates | automatic, from Let's Encrypt (ACME), stored in `private/state/` and renewed by the engine | handled by the proxy |
| Port 80 | only redirects to HTTPS and answers ACME challenges | n/a |
| Fourfold listens on | 443 (and 80) | plain HTTP on localhost, a private address, or a Unix socket |

- The engine never runs as root. It gets ports 80 and 443 through systemd socket activation or `CAP_NET_BIND_SERVICE`.
- **Several apps on one server:** only one process can listen on 443. A small **front process** (`fourfold front`, configured by the server config, owned by the admin) terminates TLS for every domain and forwards each request to that app's engine over a Unix socket. Each engine still runs as its own OS user (§1), and the front process holds no app code or data. A single-app server can skip the front process and let its engine terminate TLS directly.

#### Trusting the client's address

- Behind a proxy, every connection appears to come from the proxy, so the real client address is in `X-Forwarded-For` (or `Forwarded`). Those headers are **trusted only when the connection comes from an address listed in the config** (`config.proxy.trust`); otherwise they're ignored. Without this rule, anyone could fake their IP and slip past per-IP limits.
- The result is available to code as `req.ip`, and is the address used for the engine's own per-IP limits (sign-in, anonymous sessions, connection caps).
- **The `Host` header is checked** against the configured domain(s); a request for any other host is rejected. The engine never builds URLs from `Host` (magic links, redirects, and the CSRF `Origin` check all use `config.domain`), so a forged `Host` can't steer a sign-in link to an attacker's site.

#### Connection and request limits

Defaults, configurable in the host config within the server's ceilings:

| Limit | Default (illustrative) | Defends against |
|---|---|---|
| Time to receive request headers | 10 s | slowloris |
| Minimum body upload rate | ~1 KB/s after a grace period | slow-body attacks |
| Header size | 16 KB | oversized headers |
| Request body size | 1 MB (forms); uploads have a separate, larger cap | memory exhaustion |
| Form fields, multipart parts, JSON depth | e.g. 1000 / 100 / 64 | parser exhaustion |
| Concurrent connections, total and per IP | sized to the machine | connection floods |
| Concurrent requests (running VMs) | sized to the machine; excess waits briefly, then gets 503 | overload |
| Open SSE connections, total and per IP | counted separately, since they're long-lived | SSE floods |
| Keep-alive idle timeout | 60 s | idle connection buildup |

- Compressed request bodies aren't accepted in v1 (no decompression bombs).
- The engine parses forms, multipart uploads, and JSON itself under these limits, before any handler runs, so app code only ever sees bounded values.
- The `Server` header doesn't reveal the engine version.
- HTTP/2 is supported where TLS is.

#### Security headers by default

Sent on every response, with no developer action:

| Header | Default | Notes |
|---|---|---|
| `Strict-Transport-Security` | `max-age=31536000` | production only; `includeSubDomains` and preload are opt-in |
| `Content-Security-Policy` | see below | |
| `X-Frame-Options` | `DENY` | for old browsers; CSP `frame-ancestors` is the real control. Allowing specific embedding sites is a config option |
| `X-Content-Type-Options` | `nosniff` | |
| `Referrer-Policy` | `strict-origin-when-cross-origin` | |
| `Cross-Origin-Opener-Policy` | `same-origin` | |
| `Permissions-Policy` | camera, microphone, geolocation, etc. off | apps that need one enable it in config |

#### Content Security Policy

Because the engine renders every page, it can apply a **strict, nonce-based CSP** automatically, much as it adds CSRF tokens. It's a second wall against XSS behind the template rules: even if HTML gets in somewhere (a bug, or a careless `unsafeHTML`), a script without the nonce won't run.

```
default-src 'self'; script-src 'self' 'nonce-…'; style-src 'self' 'unsafe-inline';
img-src 'self' https: data:; connect-src 'self'; object-src 'none';
base-uri 'none'; form-action 'self'; frame-ancestors 'none'
```

- **A fresh nonce per response.** Templates are parsed at compile time, so the engine knows every `<script>` tag and adds the nonce to each one at render time, including the dev-mode live-reload script and the engine's own pages.
- **This matches the template rules already in place.** Inline event handlers (`onclick=`) are already a compile error. The same rule extends to HTMX's ways of running inline JS: `hx-on*` attributes and `js:` values in `hx-vals` are compile errors, and HTMX is configured to use no inline styles or `eval`.
- **Inline `style` attributes are allowed** (`'unsafe-inline'` for styles only), since designers use them constantly and the risk is much smaller than with scripts. A config option removes it for a stricter policy.
- **The developer can add sources** in config, e.g. a CDN for scripts or a font host. There's no option for `'unsafe-inline'` or `'unsafe-eval'` in `script-src`.
- **Violations are reported:** in dev mode, a CSP violation shows in the error overlay with the offending element; in production, it's logged to the engine log (rate-limited).
- **Fragments with scripts** (decided in M4): fragments returned to HTMX are rendered in a separate response, so a `<script>` inside one won't have the page's nonce. A fragment containing `<script>` is therefore an error when returned as a fragment response (or as `status(...)`'s body), though it's fine in a static include or a page field. HTMX is also configured not to run scripts in swapped content.

#### Dev mode

Dev mode serves plain HTTP on localhost: no HSTS, no ACME, no `Host` checks beyond localhost. Where a browser won't accept `Secure` or `__Host-` cookies over plain HTTP, dev mode uses an unprefixed, non-`Secure` session cookie; the difference is confined to dev. CSP is enforced in dev exactly as in production, so violations surface early.

#### Still open

- Whether the front process should also handle static files and caching for all apps, or stay a pure TLS router.
- Wildcard and many-domain certificates (DNS-01 challenges) for hosts with many subdomains.
- (Per-route rate limits for app endpoints use the same per-IP machinery; see §5, [Rate limits and spam](#rate-limits-and-spam).)

---

## 9. Local development

Same layout (the same partition folders), same engine, with a dev flag:

```
fourfold dev ./myapp      # serves on localhost, watches code/
```

Because local and production have the same structure, "works locally" means "works deployed." Dev mode adds:

- **Live reload:** the engine injects a tiny script into served pages that opens an SSE connection to `/__dev/reload`. After a successful recompile the engine sends a `reload` event, and the page reloads (or, if only CSS changed, swaps the stylesheet in place).
- **In-browser errors:** compile and runtime errors appear as an overlay on the page, with file, line, and the offending code highlighted.
- **Request inspector:** for each request, the template fields that were filled, the queries that ran, and the instructions and memory used against the budget.
- **REPL** against the dev instance, for poking at `data/` or calling handlers directly.
- **Fake login:** a "sign in as…" picker with test users (see §5).
- **Access report:** every URL with its effective access level, its rate limits, and the rules that produced them; also `post` handlers that read `req.form` without a `form` declaration (see §4 and §5).
- **Outbound report:** allowed hosts, secret-to-host bindings, any `"*"`, and `webhook` endpoints (see §6).
- **Jobs panel:** the queue, recent and dead jobs, the schedule, and "run now" (see §2, [Background jobs](#background-jobs)).
- **Migrations panel:** applied, pending, and failed migrations, "redo last", and "reset dev data" with seed data (see §3).

Production mode injects none of this.

---

## 10. Configuration

Throughout this document, "the host config" means the files described here.

### Two levels (the server level is optional)

- **App config**, in `myapp/private/config/`, owned by the developer. It sets that app's values.
- **Server config** (optional), e.g. `/etc/fourfold/server.ms`, owned by the admin of a host running several apps. It lists the apps (path, domain) and sets **ceilings** on budgets, session caps, outbound access, and so on.

App values must fall within the server's ceilings: the same "rules can only tighten" principle as access control and the outbound allowlist. When one person runs one app, the server config can be tiny or missing.

### Format: MiniScript, evaluated as data

Like `access.ms` and `outbound.ms`, config files are MiniScript evaluated in a restricted VM with no I/O; the output is a map. The engine reads them at startup and whenever they change. **An invalid config is handled like a failed compile:** the engine keeps the previous config and logs the error.

```
// private/config/app.ms
config = {}
config.domain = "guestbook.example.com"
config.budget = {"instructions": 10000000, "memoryMB": 64, "seconds": 10}
config.auth = {"methods": ["magicLink"], "admins": ["joe@example.com"]}
config.email = {"smtpHost": "smtp.example.com", "from": "noreply@example.com",
                "passwordSecret": "smtpPassword"}
config.session = {"capKB": 64, "anonIdleHours": 24, "userIdleDays": 14}
config.db = {"url": "postgres://db.internal/guestbook", "passwordSecret": "dbPassword"}
```

*As built in M6:* the email settings are `config.mail` (`transport`, `from`, `smtpHost`, `smtpUser`, `passwordSecret`, or Postmark's `postmarkTokenSecret`); `budget.memoryMB` is refused until there's a memory limit (R4); unknown settings are errors. The full list is in the [user guide](../user/user_guide.md#configuration-privateconfig).

### Secrets: a separate file

```
// private/config/secrets.ms
secrets = {}
secrets.weatherKey   = {"value": "abc123", "hosts": ["api.weather.gov"]}
secrets.smtpPassword = {"value": "…"}
secrets.dbPassword   = {"value": "…"}
```

- Keeping secrets separate means `app.ms` can be shared, reviewed, or checked into version control, while `secrets.ms` never is. `app.ms` refers to secrets by name only.
- **`hosts` decides visibility.** A secret with `hosts` appears in the app's `secrets` map as a host-bound handle (see §6). A secret with no `hosts` (or an empty list) is **engine-only**: used for things like SMTP and database credentials, and never visible to MiniScript code at all.
- This fails safe: forgetting `hosts` makes a secret unavailable to the app, not exposed. In dev mode, the resulting error explains why ("secret `weatherKey` exists but has no `hosts`, so app code can't use it"), and the generated `secrets.ms` template has a comment stating the rule.

### Defaults for everything

Every setting has a sensible default, so **a hobby app needs no config at all to start**. `fourfold dev` creates `private/` automatically, with localhost defaults, the fake login enabled, and placeholder secrets. In production, about the only required setting is `domain`, which the engine needs for magic-link URLs, cookie scoping, and the `Origin` check in CSRF protection.

### Settings inventory

All the settings the design has called for so far:

| Group | Settings |
|---|---|
| Site | domain |
| Budgets | instructions, memory, wall-clock time per request |
| Auth | sign-in methods, initial admins, email (SMTP/provider) |
| Sessions | size cap, idle and absolute timeouts, anonymous-session caps |
| Database | connection URL, role, credentials (by secret name) |
| Outbound | disable or cap the allowlist, allowed internal addresses, rate limit, size limit |
| Rate limits and spam | default limits for non-GET requests (anonymous and signed in); proof-of-work difficulty |
| Secrets | values and host bindings (in `secrets.ms`) |
| Logging | rotation and retention for engine logs |
| Deploy | quiet period before recompiling; maximum wait during migrations; migration budget; clearing anonymous sessions after migrations |
| Backups | schedule, retention, backup directory |
| Jobs | budget, worker count, retry limit, time zone for schedules |
| Server | TLS mode, ACME contact email, trusted proxies, connection and request limits |
| Headers | CSP additions, allowed embedding sites, HSTS options, enabled browser features |

---

## Decided against

- **App-wide hooks** (e.g. a `before(req)` in `code/app.ms` running ahead of every handler). The central access file covers the main use case, and per-handler code covers the rest.
- **A built-in editor.** Developers use their favorite text editor or IDE; the engine's dev mode (§9) provides the live feedback.

## Open questions

- Routes table format. Tentatively: a separate file from `access.ms`, with the same path syntax and the same compile-time evaluation, and optionally an inline access level per entry.
- **Background and scheduled jobs.** Designed in §2, [Background jobs](#background-jobs); remaining questions are listed there.
- **Serving on the open internet** (TLS, proxies, connection limits, security headers, CSP). Designed in §8, [Serving on the open internet](#serving-on-the-open-internet); remaining questions are listed there.
- **Schema changes and migrations.** Designed in §3, [Schema changes and migrations](#schema-changes-and-migrations); remaining questions are listed there.
- **Forms and input validation.** Designed in §4, [Forms and validation](#forms-and-validation); remaining questions are listed there.
- **Email from app code** (contact forms, notifications). A `mail` module taking structured fields, never raw headers (PHP's `mail()` header injection is the classic hole), with bodies from templates. A per-app send rate limit, and perhaps a recipient restriction (signed-in users only, or an allowlist) in the "can only tighten" style of `outbound.ms`.
- **Abuse of app endpoints** (spam). Designed in §5, [Rate limits and spam](#rate-limits-and-spam); remaining questions are listed there.
- **Testing.** A `fourfold test` command that calls handlers with synthetic requests and fake users against a throwaway `data/`. The per-request model makes this easy: a handler is nearly a pure function of the request, `session`, and `data`.
- **Non-browser clients.** `json()` exists, but CSRF and cookie sessions assume a browser. Mobile apps, scripts, and integrations need API tokens or keys, and CORS rules. Decide whether this is v1 or explicitly later.
- **Caching:** ETags and `Cache-Control` for static files, response compression, and perhaps caching rendered output.
- **Time zones and localization:** formatting is done in code (§4), so code needs good tools for dates, numbers, and translated strings.
- **Production admin console:** users, roles, sessions, job queue, and logs, since the dev tools are off in production.
- **Account deletion and data export** (GDPR and similar): ties into the auth store and data keyed by `user.id`.
- **Engine and language versioning:** an app declares which Fourfold version it targets, so engine upgrades don't silently break it.

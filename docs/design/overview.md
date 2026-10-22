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
| **Private** `private/state/` | Auth store, session store, rate-limit counters, compiled-image cache, engine logs | Read + write | None |

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

### Developer access

The developer can read, edit, move, and delete files in all the partitions, both locally and on the server. Locally, the partitions are simply folders.

---

## 2. Execution model: a fresh VM per request

Each HTTP request runs in a **fresh MiniScript VM**, discarded when the request finishes.

- Between requests, the only things that persist are the read-only code and inert data (the `data` and `session` stores). No in-memory state can build up or linger.
- **Startup cost:** the code partition is compiled once into a frozen bytecode image shared across all request VMs. Each request only allocates its own locals and globals.
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

- **The engine's part:** user ID (if signed in), created and last-used times, CSRF token. App code can't touch it.
- **The app's part:** the `session` map.

`req.user` is built from the user ID by **looking it up in the auth store on every request**. Roles are never cached in the session, so revoking a role or deleting a user takes effect immediately.

#### Load lazily, lock lazily, save only if changed

- **The engine loads the session on first access** to `session`. A request that never touches it doesn't load it and doesn't wait for any lock. That covers most static pages and many renders.
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

Redirect-after-POST almost always wants a one-time message, so v1 includes a small helper:

```
flash "Thanks for signing!"
return redirect("/guestbook")
```

The engine stores the message in the session; the next page render receives it as the `{{flash}}` field, and then it's cleared.

#### Dev mode

- The request inspector shows the session before and after each request, including its size against the cap.
- Sessions can be viewed, edited, and cleared from the dev tools.
- Each fake-login user gets its own session, so switching between "admin" and "visitor" doesn't mix them up.

---

## 3. Data storage

Three tiers, all following the core rule (data is inert; executable things live in `code/`).

### Tier 1: plain files in `data/`

- **Atomic writes:** write to a temp file, then rename it over the original, so readers never see a half-written file, even if a request is killed by its budget.
- **Locked read-modify-write:** e.g. `file.update(path, @f)` takes an exclusive lock, passes the current contents to `f`, and writes back the result. The request budget limits how long the lock can be held.
- **Append** is offered separately, since appends are naturally safe to run concurrently.
- **Value serialization that can't produce code:** a save/load (or JSON) facility for numbers, strings, lists, and maps that *refuses functions*. Loading data can never produce something callable (no equivalent of PHP's `unserialize` hole).
- Shared counters etc. should use atomic operations (`data.increment`, `data.append`, compare-and-swap), so that the simple way is also the correct way.

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

---

## 4. Pages, templates, and code

### Separating design from logic

Unlike PHP, code is **not** embedded in HTML. HTML templates contain named **template fields**, and MiniScript code lives in separate files and fills them in. This adds a little cognitive load in the simplest cases, but scales much better and lets designers own the HTML/CSS.

To keep that load low, a page and its code are paired by naming convention: `pages/guestbook.html` automatically pairs with `pages/guestbook.ms`.

### Progressive path

1. Start with pure HTML/CSS: put `about.html` in `pages/` and it's served at `/about`. No code required.
2. To add a feature, add a template field to the HTML and write a small `.ms` file next to it.
3. Larger apps add explicit routes, fragments, real-time updates, and databases.

### Routing: hybrid (decided)

- **File-based routing for pages:** `pages/about.html` (+ optional `about.ms`) is served at `/about`.
- **Static files live in `pages/` too:** anything in `pages/` that isn't `.html` or `.ms` is served as a static file at its path, e.g. `pages/members/photos/beach.jpg` at `/members/photos/beach.jpg`. Images stay next to the pages that use them, and folder access rules cover both.
- **Handlers by convention:** `POST /guestbook/sign` calls `post.sign` in `pages/guestbook.ms`. See [Handler model](#handler-model).
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
| fragment (from `frag`) | inserted as HTML | compile/render error |
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
| `form` | map of submitted form fields |
| `params` | values captured from route patterns (`{id}`) |
| `files` | uploaded files (contents held by the engine, saved to `data/` explicitly) |
| `headers` | read-only request headers |
| `user` | the signed-in user, or `null` (see §5) |
| `htmx` | true if HTMX made the request, so it expects a fragment |
| `rawBody` | the unparsed request body (`webhook` handlers only, for signature checks) |

`session` and `data` are globals, not fields of `req`. Raw cookies are left out for now, since `session` covers what beginners use cookies for.

#### Return values

| Handler returns | Response |
|---|---|
| a map (from `render` only) | the page, with its fields filled in |
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

The compiled image contains a **route table** of every URL the app can serve: pages, `get`/`post` handlers, routes-table entries (including parameterized ones like `/user/{id}`), and static files. The compiler calculates each route's effective access level once and stores it on the route entry. At request time there is no pattern matching:

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
- **Any request other than GET without a valid token is rejected before the handler runs.** The `Origin` header is checked as well.
- The developer never has to think about any of this.

GET requests are not token-checked, which is why state-changing handlers belong in `post`, not `get`. This is a documented rule, not enforced: making `data` read-only during GETs would break harmless side effects like the visitor counter in `render`.

The one exception to token checking is `webhook` handlers, which receive POSTs from outside services (see §6).

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

No VM lives beyond its request, so the core security property still holds. WebSockets could be added later if two-way connections are needed.

---

## 8. Deployment

- The developer uploads files with plain **scp/rsync**. No special tooling is required.
- **Whole-image compile and swap:** the engine compiles the entire code tree into a single image, then swaps it in atomically. If anything fails to compile, it keeps serving the old image and logs the error, so a broken or partial upload never takes the site down. Requests already running finish on the old image; new requests get the new one.
- **Wait for quiet:** the engine watches `code/` and recompiles only once files have stopped changing for a second or two. Optionally, touching a trigger file (e.g. `code/.deploy`) means "load now."
- An optional `fourfold deploy` wrapper could rsync to a staging folder and then swap directories atomically.
- **Deploys should only touch `code/`.** The docs must warn that, for example, `rsync --delete` of the whole tree from a laptop would wipe production data.

### User uploads

Files uploaded into `data/` can never execute. But if the app serves them back, uploaded `.html`/`.svg` files could still cause stored XSS in the browser. The engine's file-serving function for `data/` should:
- set `Content-Type` from an allowlist,
- send `X-Content-Type-Options: nosniff`, and
- default to `Content-Disposition: attachment` for anything that isn't a known-safe type.

Static files in `code/pages/` are served normally, since only the developer can put them there.

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
- **Access report:** every URL with its effective access level and the rules that produced it (see §5).
- **Outbound report:** allowed hosts, secret-to-host bindings, any `"*"`, and `webhook` endpoints (see §6).

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
| Secrets | values and host bindings (in `secrets.ms`) |
| Logging | rotation and retention for engine logs |
| Deploy | quiet period before recompiling |

---

## Decided against

- **App-wide hooks** (e.g. a `before(req)` in `code/app.ms` running ahead of every handler). The central access file covers the main use case, and per-handler code covers the rest.
- **A built-in editor.** Developers use their favorite text editor or IDE; the engine's dev mode (§9) provides the live feedback.

## Open questions

- Routes table format. Tentatively: a separate file from `access.ms`, with the same path syntax and the same compile-time evaluation, and optionally an inline access level per entry.

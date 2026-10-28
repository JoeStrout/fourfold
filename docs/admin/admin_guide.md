# Fourfold Admin Guide

How to set up and run a Fourfold server: installing it, serving an app, mail
for sign-in, limits, logs and backups. This guide is for the person who runs
the server. The person writing the site needs the
[user guide](../user/user_guide.md) instead; where the two overlap (the app's
config file, deploying), this guide covers what the server needs and links to
the user guide for the rest.

*Status: covers what works as of Milestone 6. Fourfold isn't ready yet to
face the internet on its own: see [Known gaps](#known-gaps). Rate limits, CGI
and FastCGI hosting, TLS and a production server will be added here as they
arrive.*

---

## Contents

1. [How Fourfold runs](#how-fourfold-runs)
2. [Installing](#installing)
3. [The app folder](#the-app-folder)
4. [Running `fourfold serve`](#running-fourfold-serve)
5. [Behind a reverse proxy](#behind-a-reverse-proxy)
6. [Keeping it running](#keeping-it-running)
7. [Mail for sign-in](#mail-for-sign-in)
8. [The first admin](#the-first-admin)
9. [Server limits: `--server-config`](#server-limits---server-config)
10. [Deploying an app's changes](#deploying-an-apps-changes)
11. [Logs](#logs)
12. [Backups](#backups)
13. [Known gaps](#known-gaps)
14. [Quick reference](#quick-reference)

---

## How Fourfold runs

One `fourfold serve` process serves one app: a folder holding the site's
code, its data and its settings. To host several apps, run one process for
each, on its own port, and have a reverse proxy send each domain to its
process.

The process:

- listens for plain HTTP, by default on `127.0.0.1:8090`. HTTPS comes from a
  reverse proxy in front of it (nginx, Caddy, Apache, a hosting service).
- needs no database server: its own state is in an SQLite file in the app
  folder.
- reads its settings from files in the app folder, and picks up changes to
  them on its own.
- loads new site code only when told to (see
  [Deploying](#deploying-an-apps-changes)), never half-way through an upload.

`fourfold dev` is the same engine, set up for a site author working on their
own computer. Don't use it on a server: it listens only on `localhost`, shows
error details to visitors, prints sign-in emails instead of sending them, and
offers a "sign in as anyone" menu.

---

## Installing

Fourfold is built from source. See [building.md](../building.md) for the
requirements (CMake, Ninja, a C++17 compiler, libcurl, and the MiniScript 2
sources). For a server, use the `release` preset:

```sh
cmake --preset release
cmake --build --preset release
./build/release/bin/fourfold version
```

The result is one program, `build/release/bin/fourfold`. Everything it serves
itself (htmx, the default Markdown style) is built in, so there's nothing to
install beside it. Copy it to wherever you keep programs, such as
`/usr/local/bin/`.

On Linux, the server needs libcurl at run time (on Debian or Ubuntu, the
`libcurl4` package; `libcurl4-openssl-dev` brings it in on a build machine).
On macOS it's part of the system.

---

## The app folder

```
myapp/
  code/            the site: pages, templates, code, images (the author's)
  data/            what the site stores while running (the app's)
  dropbox/         the app's own output: print.log, reports (the app's)
  private/
    config/        app.ms and secrets.ms: settings and passwords
    state/         the engine's own files (never the app's)
      state.db     sessions, users, roles, sign-in links (SQLite)
      secret.key   the key that signs CSRF tokens
      logs/        the engine log, one file per day
```

If any of these folders is missing, `fourfold serve` creates it. The ones
under `private/`, and `data/` and `dropbox/`, are made readable only by their
owner (mode 0700).

**Run Fourfold as its own user**, not as root, and have that user own the
whole app folder.

**Symbolic links.** One of the folders above may itself be a link (`code/`
pointing to a checkout elsewhere, say), but no folder may lead into another
(`code/` into `data/`), and inside them, links aren't followed: a link in
`code/` is skipped with a warning, and one in `private/config/` is an error.

**Who changes what:**

| Folder | Changed by | Fourfold |
|---|---|---|
| `code/` | the site's author | reads it; never writes it |
| `data/` | the app, while running | reads and writes it |
| `dropbox/` | the app writes; you read | appends to it |
| `private/config/` | you, or the author | reads it, and watches it for changes |
| `private/state/` | Fourfold only | reads and writes it |

Nothing in `data/`, `dropbox/` or `private/` is ever served by URL, whatever
it's named.

**Keep `secrets.ms` private.** It holds passwords in plain text. Don't put it
in version control, and don't copy it into anywhere a web server can read.
The `app.ms` beside it can be shared: it only names secrets.

---

## Running `fourfold serve`

```sh
fourfold serve /srv/club --host club.example.com
```

| Option | Default | Is |
|---|---|---|
| `--listen ADDRESS:PORT` | `127.0.0.1:8090` | where to listen. An IPv6 address goes in brackets: `[::1]:8090` |
| `--host NAME` | none | a host name the site answers to; give it more than once for several (`example.com`, `www.example.com`) |
| `--plain-http` | off | the site is reached over plain HTTP, not HTTPS (see below) |
| `--server-config FILE` | none | limits on the app's settings (see [Server limits](#server-limits---server-config)) |

**Host names.** Fourfold answers only requests for its own host names, the
`--host` names and the app's `config.domain` (from
`private/config/app.ms`). Any other `Host` header gets a 400. If neither is
given, it answers only to `localhost`, and won't listen on any address but
`127.0.0.1` or `::1`.

**HTTPS.** Fourfold assumes visitors reach it over HTTPS (from the proxy), so
it marks its cookies `Secure`, and browsers send them only over HTTPS. If
visitors really do use plain HTTP, say on a private network, give
`--plain-http`; then cookies aren't marked `Secure`, and sign-in links start
with `http://`. Use it only on a network you trust: anyone on it can read a
visitor's session cookie.

**Startup checks.** `fourfold serve` won't start if:

- the app's config (`private/config/app.ms` or `secrets.ms`) has an error.
  The errors are printed, with file and line. (`fourfold dev` starts with the
  defaults instead.)
- `auth.fakeLogin` is on in the config.
- mail is set up but `config.domain` isn't (sign-in links need it).
- it's listening beyond `localhost` with no host name.
- the server config (`--server-config`) can't be read or has an error.

If the site's code doesn't compile, the server still starts, and serves an
error until a deploy fixes it. The errors are in the engine log.

When it starts, it prints what it's serving, and reminds you of the
[known gaps](#known-gaps).

---

## Behind a reverse proxy

The proxy handles HTTPS and sends each request on to Fourfold. It must pass
the original `Host` header along, since Fourfold checks it.

**Caddy** (gets its own certificates):

```
club.example.com {
    reverse_proxy 127.0.0.1:8090
}
```

**nginx:**

```nginx
server {
    listen 443 ssl;
    server_name club.example.com;
    # ssl_certificate ...; ssl_certificate_key ...;

    client_max_body_size 1m;
    location / {
        proxy_pass http://127.0.0.1:8090;
        proxy_set_header Host $host;
    }
}
```

Fourfold limits request bodies to 1 MB itself; the nginx setting just
refuses bigger ones sooner.

**Visitor addresses.** Fourfold doesn't yet read `X-Forwarded-For`, so behind
a proxy every visitor seems to come from the proxy's address. That matters
for two limits that count by visitor address:

- **Sign-in links:** 10 per address an hour. Behind a proxy, that's 10 an
  hour for the whole site. (The limit of 3 an hour per email address still
  works as meant.)
- **Anonymous sessions:** 1,000 per address. Behind a proxy, that's 1,000 for
  the whole site, and the oldest are ended to make room for new ones.

A setting for a trusted proxy comes with M7 (see [Known gaps](#known-gaps));
until then, a busy site behind a proxy will hit these limits.

---

## Keeping it running

Fourfold doesn't run itself in the background. Use your system's service
manager. On Linux with systemd, `/etc/systemd/system/fourfold-club.service`:

```ini
[Unit]
Description=Fourfold: club.example.com
After=network.target

[Service]
User=fourfold
ExecStart=/usr/local/bin/fourfold serve /srv/club --host club.example.com
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Then `systemctl enable --now fourfold-club`. The startup message and any
config errors go to the journal (`journalctl -u fourfold-club`).

**Stopping.** There's no graceful shutdown yet: stopping the process ends it
at once, and requests in progress get no response. What's stored is safe:
`data/` files are replaced whole (written aside, then renamed), and
`state.db` is SQLite, which survives being stopped mid-write. The last few
engine log lines may be lost.

**Restart after** changing `config.domain`, a `--host`, or the server config;
those are read only at startup. Everything else in `private/config/` is
picked up while running.

---

## Mail for sign-in

Visitors sign in by email: they enter their address, and Fourfold sends a
link that signs them in. So a site with sign-in needs a way to send mail.
Without one, the sign-in page says that sign-in by email isn't available, and
everything else works.

Mail is set up in the app's `private/config/app.ms`, with the password in
`secrets.ms`. Both are read again within a second or so of a change, so no
restart is needed (except for `domain`).

```
// private/config/app.ms
config = {}
config.domain = "club.example.com"
config.mail = {"transport": "smtp", "from": "Club <noreply@club.example.com>",
    "smtpHost": "smtp.example.com", "smtpUser": "noreply@club.example.com",
    "passwordSecret": "smtpPassword"}
```

```
// private/config/secrets.ms
secrets = {}
secrets.smtpPassword = {"value": "the password"}
```

**`domain` is required** with mail: sign-in links always point to
`https://<domain>/`, never to whatever host name a request named, so no one
can have a link sent that points elsewhere.

**The `from` address** must be one your mail provider lets you send from;
most also want the domain's SPF and DKIM records set up, or the mail goes to
spam. `"noreply@club.example.com"` or `"Club <noreply@club.example.com>"`.

### SMTP

| Setting | Default | Is |
|---|---|---|
| `smtpHost` | | the mail server's host name |
| `smtpPort` | 587 (465 with `"tls"`) | its port |
| `smtpSecurity` | `"starttls"` | `"starttls"`: plain connection, then upgraded to TLS, which is required. `"tls"`: TLS from the start (usually port 465). `"none"`: no encryption, allowed only for a server on this machine (`localhost`) |
| `smtpUser` | | the login, if the server needs one |
| `passwordSecret` | | the name of the secret holding the password (needed with `smtpUser`) |

The server's certificate is checked against the system's trusted
certificates. A server that doesn't offer STARTTLS is refused, rather than
sending the link in the clear.

To use a mail server on the same machine (a local Postfix relay, say):

```
config.mail = {"transport": "smtp", "from": "noreply@club.example.com",
    "smtpHost": "localhost", "smtpPort": 25, "smtpSecurity": "none"}
```

### Postmark

```
config.mail = {"transport": "postmark", "from": "noreply@club.example.com",
    "postmarkTokenSecret": "postmarkToken"}
```

```
secrets.postmarkToken = {"value": "your Postmark server token"}
```

`postmarkStream` chooses the message stream (default `"outbound"`, the
transactional one). The `from` address must be a sender signature or domain
verified in Postmark.

### Secrets

A secret is a map with a `value`. A secret the engine uses for mail must
have no `hosts`: a secret with `hosts` is meant for the app's own code (a
later feature), and Fourfold won't use one for both. So the app's code can
never see the mail password.

### Testing it

1. Set up `domain`, `mail` and the secret as above. If anything's wrong,
   the error goes to the engine log, and the old config stays in use (at
   startup, `fourfold serve` prints it and stops).
2. Open `https://<domain>/__auth/login`, enter an address you can read, and
   send the link.
3. The page says "check your email" whatever happens, so it doesn't reveal
   who has an account. Look in the engine log for an `auth` line: either
   "sign-in link sent to ...", or "sending a sign-in link to ... failed",
   with the reason (what the mail server answered, a refused certificate,
   and so on).

Each address can be sent 3 links an hour, and each visitor address 10 (but
see [Behind a reverse proxy](#behind-a-reverse-proxy)). A link works once,
for 15 minutes (`auth.linkMinutes` changes that).

You can check the mail settings before going live with `fourfold dev` on a
copy of the app: with `config.mail` set up, dev mode sends real mail too,
and links point to `localhost`.

---

## The first admin

Users are created when they first sign in. Roles (like `"admin"`) are given
by the app's own code, through its `users` functions, so someone has to be
an admin first. That's `auth.admins` in the config:

```
config.auth = {"admins": ["you@example.com"], "signUp": "open"}
```

An address in `admins` gets the `"admin"` role each time it signs in. It
stays an admin while it's in the list; removing it from the list doesn't take
the role away (an admin, through the app, can).

`auth.signUp` decides who may sign in:

- `"open"` (the default): anyone with an email address. Their first sign-in
  makes them a user with no roles.
- `"closed"`: only users who already exist, and the addresses in `admins`.

To run a closed site, open it while the first users sign in, then close it;
or list everyone who needs to sign in under `admins` for now. (A way for an
admin to add users without opening sign-up will come later.)

---

## Server limits: `--server-config`

An app sets its own limits on each request (instructions, seconds) and on
sessions (size, how long they last, how many). On a server shared with other
people's apps, you can set **ceilings** on those: an app value above one is
lowered to it, with a warning in the log. A file given with
`--server-config`, MiniScript like the app's config, sets a map called
`server`:

```
// /etc/fourfold/server.ms
server = {}
server.ceilings = {}
server.ceilings.budget = {"instructions": 50000000, "seconds": 20}
server.ceilings.session = {"capKB": 64, "userMaxDays": 30}
```

| Ceiling | The app's setting, and its default |
|---|---|
| `budget.instructions` | instructions per request: 10,000,000 |
| `budget.seconds` | seconds per request, including waiting: 10 |
| `session.capKB` | the largest session: 64 KB |
| `session.anonIdleHours`, `anonMaxDays` | when an anonymous session ends: 24 hours idle, 7 days at most |
| `session.userIdleDays`, `userMaxDays` | when a signed-in session ends: 14 days idle, 30 at most |
| `session.anonPerAddress`, `anonTotal` | anonymous sessions per visitor address, and in all: 1,000; 100,000 |

Any you leave out isn't limited. As with the app's config, a misspelled name
is an error, and an error stops `fourfold serve` from starting. The file is
read only at startup; restart to change it. It needn't be in the app folder,
and the app can't read it.

---

## Deploying an app's changes

The author's changes go into `code/`; then `touch code/.deploy` makes
Fourfold compile and switch to them, without a restart and without dropping
a request. If the new code has an error, the old version keeps serving, and
the errors go to the engine log. See
[Deploying changes](../user/user_guide.md#deploying-changes) in the user
guide.

A deploy doesn't touch `data/`, `dropbox/`, sessions or users. Config changes
need no deploy.

---

## Logs

**The engine log**, `private/state/logs/engine-YYYY-MM-DD.log`, one file per
day (UTC), one line per event:

```
2026-10-25T20:31:02.123Z csrf POST /guestbook/sign: no CSRF cookie
```

The word after the time says what kind of event it is:

| Word | Is |
|---|---|
| `start` | the server started |
| `request` | a request, with the visitor's address and the response's status |
| `error` | a script error: message, file and line |
| `compile` | a deploy's result, and compile errors |
| `config` | a changed config loaded, or refused (with its errors) |
| `csrf` | a refused form post |
| `session` | session problems, like one too big to save |
| `auth` | sign-in links sent, refused, throttled or failed to send; sign-ins and sign-outs; new users |
| `mail` | the whole message, when `mail.transport` is `"log"` |

Control characters in a message are escaped, so nothing logged can fake a
line. The app can't write to this log. Old log files aren't removed yet;
clear them out yourself (with a cron job, say) until rotation arrives.

**The app's output**, in `dropbox/`: what its code `print`s goes to
`dropbox/print.log`, and the app may write reports there. The site's author
may want to see these.

---

## Backups

Back up the whole app folder. What matters most:

- `data/`: the site's own data. Each file is replaced whole when it changes,
  so copying the folder at any moment gets complete files (though a file
  changed during the copy may be newer than its neighbors).
- `private/state/state.db`: users and their roles. Copy it with SQLite's own
  backup, which is safe while Fourfold is running:

  ```sh
  sqlite3 /srv/club/private/state/state.db ".backup /backups/club-state.db"
  ```

  A plain file copy while it's running can miss recent changes, which are in
  `state.db-wal` beside it.
- `private/config/`: the settings, and `secrets.ms`. Keep its backups as
  private as the original.
- `code/`: usually also kept elsewhere by the author, but cheap to keep.

`secret.key` needn't be backed up: if it's lost, a new one is made, and the
only effect is that forms open in browsers at that moment have to be
reloaded. Sessions are in `state.db`, so restoring an old copy signs everyone
out who signed in since.

To restore: stop Fourfold, put the files back, start it.

---

## Known gaps

`fourfold serve` prints these when it starts. Until they're filled, keep a
Fourfold site behind a proxy, and expect to watch it.

- **No TLS of its own.** HTTPS has to come from a reverse proxy (M11).
- **No per-request memory limit.** Time and instruction limits apply, but a
  script can use as much memory as the machine has. A memory limit for the
  process from the service manager (`MemoryMax=` in systemd) at least
  protects everything else on the machine.
- **No rate limits or spam protection** beyond the sign-in link throttle
  (M7).
- **Visitor addresses behind a proxy** aren't known (no `X-Forwarded-For`),
  so per-address limits count everyone together (M7). See
  [Behind a reverse proxy](#behind-a-reverse-proxy).
- **No graceful shutdown, and no log rotation** (M11).
- **CGI and FastCGI hosting**, for shared web hosts, aren't ready yet (M10).

---

## Quick reference

### Commands

| Command | Does |
|---|---|
| `fourfold serve <folder> [options]` | serve an app (see [Running](#running-fourfold-serve)) |
| `fourfold dev <folder> [--port N]` | serve an app on `localhost`, for its author |
| `fourfold version` | print the version |
| `fourfold help` | list the commands |

### Files

| File | Is |
|---|---|
| `code/.deploy` | touch it to load changed code |
| `private/config/app.ms` | the app's settings (the `config` map) |
| `private/config/secrets.ms` | passwords and tokens (the `secrets` map) |
| `private/state/state.db` | sessions, users, roles, sign-in links |
| `private/state/secret.key` | signs CSRF tokens; made if missing |
| `private/state/logs/` | the engine log |
| `dropbox/print.log` | the app's `print` output |
| the `--server-config` file | ceilings on the app's settings (the `server` map) |

### Config settings for admins

The settings in `private/config/app.ms` an admin usually sets; the rest are
in the user guide's [Configuration](../user/user_guide.md#configuration-privateconfig).

| Setting | Default | Is |
|---|---|---|
| `domain` | none | the site's host name: answered to, and used in sign-in links |
| `auth.admins` | `[]` | addresses given `"admin"` when they sign in |
| `auth.signUp` | `"open"` | `"open"` or `"closed"` |
| `auth.methods` | `["magicLink"]` | `[]` turns sign-in off |
| `auth.linkMinutes` | 15 | how long a sign-in link works |
| `mail.transport` | `"none"` | `"smtp"`, `"postmark"`, `"log"` (to the engine log), or `"none"` |
| `mail.from` | | the sender address |
| `mail.smtpHost`, `smtpPort`, `smtpSecurity`, `smtpUser`, `passwordSecret` | port 587, `"starttls"` | SMTP |
| `mail.postmarkTokenSecret`, `postmarkStream` | `"outbound"` | Postmark |

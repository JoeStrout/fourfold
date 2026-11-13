# Fourfold Admin Guide

How to set up and run a Fourfold server: installing it, serving an app, mail
for sign-in, limits, logs and backups. This guide is for the person who runs
the server. The person writing the site needs the
[user guide](../user/user_guide.md) instead; where the two overlap (the app's
config file, deploying), this guide covers what the server needs and links to
the user guide for the rest.

*Status: covers what works as of Milestone 11: Fourfold can face the
internet on its own (HTTPS with certificates from Let's Encrypt, HTTP/2, and
limits on slow and abusive clients), or sit behind a reverse proxy, or run
on shared hosting through CGI and FastCGI. What's still missing is in
[Known gaps](#known-gaps). Hosting several apps on one server, each as its
own OS user, comes next (M11.1).*

---

## Contents

1. [How Fourfold runs](#how-fourfold-runs)
2. [Installing](#installing)
3. [The app folder](#the-app-folder)
4. [Running `fourfold serve`](#running-fourfold-serve)
5. [On the internet: `--tls`](#on-the-internet---tls)
6. [Behind a reverse proxy](#behind-a-reverse-proxy)
7. [Connection limits](#connection-limits)
8. [Security headers](#security-headers)
9. [Rate limits](#rate-limits)
10. [Outbound requests and app mail](#outbound-requests-and-app-mail)
11. [Background jobs and live updates](#background-jobs-and-live-updates)
12. [Keeping it running](#keeping-it-running)
13. [Shared hosting: CGI and FastCGI](#shared-hosting-cgi-and-fastcgi)
14. [Mail for sign-in](#mail-for-sign-in)
15. [The first admin](#the-first-admin)
16. [Server limits: `--server-config`](#server-limits---server-config)
17. [Deploying an app's changes](#deploying-an-apps-changes)
18. [Migrations: `fourfold migrate`](#migrations-fourfold-migrate)
19. [A Postgres database](#a-postgres-database)
20. [Logs](#logs)
21. [Backups](#backups)
22. [Known gaps](#known-gaps)
23. [Quick reference](#quick-reference)

---

## How Fourfold runs

One `fourfold serve` process serves one app: a folder holding the site's
code, its data and its settings. To host several apps, run one process for
each, on its own port, and have a reverse proxy send each domain to its
process.

The process:

- either faces the internet itself (`--tls`): HTTPS on port 443, with
  certificates it gets and renews from Let's Encrypt, and port 80 sending
  visitors to HTTPS;
- or listens for plain HTTP, by default on `127.0.0.1:8090`, behind a
  reverse proxy that provides HTTPS (nginx, Caddy, Apache, a hosting
  service, a cloud load balancer).
- needs no database server: its own state is in an SQLite file in the app
  folder, and so is the app's database, unless you give it a Postgres one.
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

**Postgres support** is built in if the build machine has libpq (on Debian
or Ubuntu, `libpq-dev`; with Homebrew, `postgresql` or `libpq`); the server
then needs libpq at run time (`libpq5`). Without it, everything else works,
and an app that asks for Postgres is refused at startup. The CMake output
says which: "Postgres support: yes".

---

## The app folder

```
myapp/
  code/            the site: pages, templates, code, images (the author's)
  data/            what the site stores while running (the app's)
    .fourfold/     the app's SQLite database (app.sqlite), and which migrations
                   have run on these files
  dropbox/         the app's own output: print.log, reports (the app's)
  private/
    config/        app.ms and secrets.ms: settings and passwords
    state/         the engine's own files (never the app's)
      state.db     sessions, users, roles, sign-in links (SQLite)
      secret.key   the key that signs CSRF tokens
      logs/        the engine log, one file per day
      backups/     backups (unless config.backups.folder puts them elsewhere)
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
| `--tls` | off | serve HTTPS itself (see [On the internet](#on-the-internet---tls)) |
| `--listen ADDRESS:PORT` | `127.0.0.1:8090`; with `--tls`, `[::]:443` | where to listen. An IPv6 address goes in brackets: `[::1]:8090`; `[::]` is every address, IPv4 too |
| `--redirect-listen ADDRESS:PORT` | `[::]:80` | with `--tls`: where port 80's redirects and Let's Encrypt's checks are answered |
| `--no-redirect` | | with `--tls`: no port 80 (then certificates must come from files) |
| `--host NAME` | none | a host name the site answers to; give it more than once for several (`example.com`, `www.example.com`) |
| `--plain-http` | off | the site is reached over plain HTTP, not HTTPS (see below) |
| `--server-config FILE` | none | limits on the app's settings (see [Server limits](#server-limits---server-config)) |
| `--migrate` | off | apply pending migrations before serving (see [Migrations](#migrations-fourfold-migrate)) |
| `--allow-older-code` | off | the emergency override: serve code older than the data's migrations |

**Host names.** Fourfold answers only requests for its own host names, the
`--host` names and the app's `config.domain` (from
`private/config/app.ms`). Any other `Host` header gets a 400. If neither is
given, it answers only to `localhost`, and won't listen on any address but
`127.0.0.1` or `::1`.

**HTTPS.** Without `--tls`, Fourfold assumes visitors reach it over HTTPS
(from the proxy), so
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
- the app has migrations that haven't been applied (a fresh install has all
  of them): run `fourfold migrate` first, or start with `--migrate`. See
  [Migrations](#migrations-fourfold-migrate).
- another Fourfold (`serve` or `dev`) is already running the same app.

If the site's code doesn't compile, the server still starts, and serves an
error until a deploy fixes it. The errors are in the engine log.

When it starts, it prints what it's serving, and reminds you of the
[known gaps](#known-gaps).

---

## On the internet: `--tls`

On a server of its own (a VPS, say), Fourfold needs nothing in front of it:

```sh
fourfold serve /srv/club --tls
```

with the site's name in its config:

```
// private/config/app.ms
config = {}
config.domain = "club.example.com"
config.tls = {"email": "you@example.com"}   // optional: Let's Encrypt's contact for the account
```

It then:

- listens for HTTPS on port 443 (every address, IPv4 and IPv6), with
  HTTP/2 for browsers that offer it;
- on port 80, sends every request to the same page over HTTPS (a 301, or
  308 for a form post), and answers Let's Encrypt's checks;
- gets a certificate for `config.domain` and each `--host` name from
  [Let's Encrypt](https://letsencrypt.org) when it starts, and renews it
  when it has 30 days (or a third of its life) left, checking twice a day.
  Using Let's Encrypt means agreeing to its
  [subscriber agreement](https://letsencrypt.org/repository/).

Before the first start, point the name's DNS (A and AAAA records) at the
server, and open ports 80 and 443 in its firewall: Let's Encrypt checks the
name by fetching a file from port 80. Until the first certificate arrives
(a few seconds), HTTPS connections fail; if it can't get one, the log says
why (`tls` lines), and it tries again after 1 minute, 10 minutes, an hour,
then every 6 hours. A certificate that can't be renewed keeps being served
until it expires, so there are weeks to notice.

**Trying it out.** Let's Encrypt limits how many certificates a name gets
a week. While experimenting, use its staging service, whose certificates
browsers don't trust but which has generous limits:

```
config.tls = {"acme": "letsencrypt-staging"}
```

Then remove the setting (and `private/state/tls/`) for real certificates.
`"acme"` can also be any other CA's ACME directory URL.

**Your own certificate.** To use a certificate from elsewhere (a company
CA, a wildcard certificate), put its PEM files in `private/config/` and
name them:

```
config.tls = {"certFile": "cert.pem", "keyFile": "key.pem"}
```

The certificate file holds the site's certificate followed by its
intermediates. Fourfold reads them at startup (restart after renewing
them), and warns when the certificate has less than two weeks left.

**Where certificates live.** In `private/state/tls/`: the ACME account's
key and record, and each certificate with its key (readable by Fourfold's
user only). Backups include them; deleting the folder just means new ones
are fetched.

**What visitors get.** TLS 1.2 and 1.3 only, with forward-secret,
authenticated ciphers (Mozilla's "intermediate" profile), and
`Strict-Transport-Security` on every response (see
[Security headers](#security-headers)). `config.tls.http2 = false` turns
HTTP/2 off.

**Ports below 1024 without root.** Fourfold never needs to run as root.
Either let systemd open the ports and hand them over (socket activation;
see [Keeping it running](#keeping-it-running)), or give the binary the one
privilege it needs:

```sh
sudo setcap cap_net_bind_service=+ep /usr/local/bin/fourfold
```

(`setcap` must be run again whenever the binary is replaced.)

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

    client_max_body_size 33m;
    location / {
        proxy_pass http://127.0.0.1:8090;
        proxy_set_header Host $host;
    }
}
```

Fourfold limits request bodies itself: 1 MB for forms, and up to 33 MB for
forms with file uploads, within what each form declares. The nginx setting
just refuses bigger ones sooner.

**Visitor addresses.** Behind a proxy, every connection comes from the
proxy's address, so Fourfold has to be told to believe the proxy about who
the visitor is. List the proxy's address in the app's config:

```
// private/config/app.ms
config.proxy = {"trust": ["127.0.0.1"]}
```

Addresses and ranges both work (`"10.0.0.0/8"`, `"::1"`). From those
addresses only, Fourfold reads the standard `Forwarded` header if there is
one, else `X-Forwarded-For`, from the right: the rightmost address that
isn't a trusted proxy is the visitor. From anywhere else the headers are
ignored, so a visitor can't claim another address to slip past a limit. A
trusted proxy's connections also aren't counted against the per-address
[connection limits](#connection-limits). Caddy adds `X-Forwarded-For` on
its own; nginx needs:

```nginx
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
```

Without the setting, every visitor seems to come from the proxy, and the
limits that count by address count the whole site together: rate limits
(see [Rate limits](#rate-limits)), sign-in links (10 per address an hour),
and anonymous sessions (1,000 per address).

**Trust only your own proxy.** If the list includes an address anyone can
send from, anyone can choose their address.

---

## Connection limits

Whether it faces the internet or sits behind a proxy, Fourfold holds every
client to limits, so a few slow or greedy ones can't tie it up:

| Limit | Default | Setting (`config.server`) |
|---|---|---|
| Time to send a request's headers (and, over TLS, to finish the handshake) | 10 seconds; then 408 | `headerSeconds` |
| A request body's speed, after its first 10 seconds | 1 KB a second on average; then 408 | `minUploadBytesPerSecond` |
| A keep-alive connection sitting idle | 60 seconds | `keepAliveSeconds` |
| Connections open at once | what the open-file limit allows, less 256 | `connections` |
| Connections from one address | 100 (a trusted proxy's, and local ones, aren't counted) | `connectionsPerAddress` |
| Requests being handled at once | 64; more wait, up to 5 seconds, then 503 | `requests`, `requestWaitSeconds` |
| Open live-update streams (server-sent events) | 1,000; 20 per visitor address; then 503 | `streams`, `streamsPerAddress` |
| Request headers | 16 KB, 100 fields | |
| Request bodies | 1 MB; 33 MB for forms with file uploads | |
| HTTP/2 streams at once, per connection | 100 | |

For example:

```
// private/config/app.ms
config.server = {"connectionsPerAddress": 50, "requests": 128}
```

These are read at startup; restart to change them. On a shared server, the
server config can put ceilings on `connections`, `connectionsPerAddress`,
`streams`, `streamsPerAddress` and `requests` (see
[Server limits](#server-limits---server-config)).

Fourfold raises its own open-file limit to the system's hard limit (up to
65,536) when it starts. With systemd, `LimitNOFILE=65536` makes sure the
hard limit is that high.

---

## Security headers

Every response carries these, with nothing to set:

| Header | Sent |
|---|---|
| `Content-Security-Policy` | a strict policy: scripts only from the site, or inline ones the engine wrote (each response's own nonce) |
| `Strict-Transport-Security` | `max-age=31536000` (a year), over HTTPS in `fourfold serve` |
| `X-Frame-Options` | `DENY` (other sites can't put the site in a frame) |
| `X-Content-Type-Options` | `nosniff` |
| `Referrer-Policy` | `strict-origin-when-cross-origin` |
| `Cross-Origin-Opener-Policy` | `same-origin` |
| `Permissions-Policy` | camera, microphone, location, payment, USB, serial, Bluetooth and screen capture off |

The site's author adjusts them in `config.headers` (sources to add to the
policy, sites allowed to frame it, HSTS's options, browser features to
allow); see the user guide's
[JavaScript and the Content Security Policy](../user/user_guide.md#javascript-and-the-content-security-policy).

**Policy violations.** Browsers report scripts and styles the policy
blocked to `/__fourfold/csp-report`; each report is a `csp` line in the
engine log (30 a minute at most, then a count of the rest). A burst of them
after a deploy usually means the site needs a source added; out of the blue,
it may mean someone is trying to inject a script.

**HSTS, once sent, sticks:** browsers remember for a year to use only
HTTPS for the site. Before turning on `includeSubDomains` or `preload`, be
sure every subdomain has HTTPS.

---

## Rate limits

Every request other than GET is counted per visitor address (60 a minute for
visitors who aren't signed in) or per user (300 a minute for those who are).
Over a limit, the visitor gets a 429 with `Retry-After`. The site's author
may add stricter rules per page in `code/limits.ms`, a timing check, and a
proof-of-work puzzle; see the user guide's
[Spam and rate limits](../user/user_guide.md#spam-and-rate-limits-codelimitsms).

The defaults are in the app's config:

```
config.limits = {"anonymous": "60/minute", "signedIn": ["300/minute", "10000/day"]}
```

Counters are kept in `private/state/state.db`, so they hold across restarts.
Refusals are logged (`limits` and `spam` in the [engine log](#logs)). A
school or office behind one address counts as one visitor, so raise
`anonymous` if your visitors are like that, or have them sign in.

---

## Outbound requests and app mail

An app's code can call other servers (`http`) and send its own mail
(`mail.send`); see the user guide's
[Calling other servers](../user/user_guide.md#calling-other-servers-http) and
[Sending email](../user/user_guide.md#sending-email-mailsend). The app lists
the hosts it may reach, and the addresses besides its users it may mail, in
`code/outbound.ms`. Without that file it can reach nothing.

What the app can never do, whatever it lists: connect to a private or
internal address (loopback, your private networks, link-local addresses
such as the cloud metadata service at 169.254.169.254, and the like). The
engine checks the address it connects to, every time, so a name that points
there is refused too. If the app really needs a service on your network,
allow that address, and only it:

```
config.outbound = {"allowAddresses": ["10.0.5.20"]}
```

Your settings can only tighten what the app asks for:

```
config.outbound = {
    "enabled": true,                       // false: no outbound requests at all
    "allow": ["api.weather.gov"],          // a cap: the app's list must fit within it
    "perMinute": 120,                      // requests a minute, for the whole app
    "maxResponseKB": 5120,                 // after decompression
    "timeoutSeconds": 10,                  // one request, redirects included
}
config.mail = {..., "appMail": true, "appPerHour": 100}   // mail.send on or off, and its hourly limit
```

**Secrets for the app's code** go in `private/config/secrets.ms` like the
engine's, with where they may be used:

```
secrets.weatherKey = {"value": "...", "hosts": ["api.weather.gov"]}   // sent only to these hosts
secrets.stripeHook = {"value": "whsec_...", "verify": true}           // for checking webhook signatures
```

The app's code holds only a handle to each: it can send the key to those
hosts, or check a signature with it, and nothing else. It can't read it,
show it, store it, or send it anywhere else. Secrets with neither `hosts`
nor `verify` stay the engine's own.

Each outbound request goes in the [engine log](#logs) (`outbound`): method,
URL without its query string, status, time and size, or why it was refused.
App mail is logged too (`mail`). The app's `webhook.x` handlers take posts
from other servers without a CSRF token (they check a signature instead);
access rules and rate limits still apply to them. A busy webhook caller
(a payment provider sending bursts) counts against `limits.anonymous` like
anyone else, from its address; raise that limit if it's refused.

---

## Background jobs and live updates

An app can run **jobs** in the background (`code/jobs/`), on a schedule
(`code/schedule.ms`) or when its code enqueues them, and push **live
updates** to open pages (over server-sent events, on `/live/<channel>`).
See the user guide's
[Background jobs](../user/user_guide.md#background-jobs-codejobs) and
[Live updates](../user/user_guide.md#live-updates-publish).

**Jobs** run inside `fourfold serve`, on their own thread, with nothing to
set up. The queue is in `private/state/state.db`; a job interrupted by a
restart runs again when the server starts, and a scheduled time missed while
the server was down runs once at startup. The settings:

```
config.jobs = {
    "timeZone": "America/Denver",   // the schedule's zone (default "UTC")
    "workers": 2,                   // jobs running at once
    "instructions": 100000000,      // each run's limits
    "seconds": 60,
    "maxAttempts": 5,               // tries before a failing job is dead
}
```

The time zone is read from the system's zoneinfo files
(`/usr/share/zoneinfo`, or `$TZDIR`), so the server needs them: on Debian
and Ubuntu, the `tzdata` package. An unknown zone is a config error.
Scheduled times follow the zone's daylight saving changes. Each job run, and
each failure, is in the [engine log](#logs) (`jobs`). Dead jobs (failed
`maxAttempts` times) stay in the queue; the jobs panel that lists them is
dev mode's, so on a server, read the log for now.

**Live updates** hold a connection open for each subscribed page. They count
against the server's connection limit (1,024); at most 1,000 pages may
subscribe at once, after which new ones get a 503 and retry. Through a
reverse proxy, the stream must not be buffered or cut off for being idle:

```nginx
    location /live/ {
        proxy_pass http://127.0.0.1:8090;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_buffering off;
        proxy_read_timeout 1h;
    }
```

(Fourfold sends `X-Accel-Buffering: no`, which nginx honors anyway, and a
comment every 15 seconds to keep the connection alive. Caddy needs nothing.)
Who may subscribe to a channel is up to the app's access rules.

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

**With `--tls`, as a user with no privileges,** let systemd open ports 443
and 80 and hand them over (socket activation).
`/etc/systemd/system/fourfold-club.socket`:

```ini
[Unit]
Description=Fourfold's ports: club.example.com

[Socket]
# In this order: HTTPS first, then port 80.
ListenStream=443
ListenStream=80

[Install]
WantedBy=sockets.target
```

And `/etc/systemd/system/fourfold-club.service`:

```ini
[Unit]
Description=Fourfold: club.example.com
After=network.target
Requires=fourfold-club.socket

[Service]
User=fourfold
ExecStart=/usr/local/bin/fourfold serve /srv/club --tls
Restart=on-failure
MemoryMax=2G
LimitNOFILE=65536
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/srv/club
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

Then `systemctl enable --now fourfold-club.socket fourfold-club`. Fourfold
uses the sockets it's given in place of `--listen` and `--redirect-listen`:
the first for HTTPS and the second for port 80, or by name, if the socket
units set `FileDescriptorName=https` and `http`.

**Stopping.** `systemctl stop` (SIGTERM) or Ctrl-C stops it gracefully: it
stops taking connections, closes idle ones, ends live-update streams (the
browsers reconnect), and lets requests in progress finish, for up to 30
seconds; then it exits. A second signal stops it at once. Even then, what's
stored is safe: `data/` files are replaced whole (written aside, then
renamed), and `state.db` is SQLite, which survives being stopped mid-write.

**Restart after** changing `config.domain`, a `--host`, `config.tls`,
`config.server`, or the server config; those are read only at startup. Everything else in `private/config/` is
picked up while running.

---

## Shared hosting: CGI and FastCGI

On shared web hosting, Apache is already running, and you can't run a server
of your own. Fourfold can still serve an app there, through CGI (a
`fourfold` process for each request) or FastCGI (a `fourfold` process that
Apache starts and keeps for many requests). It's the same engine, with a few
things missing (below); the standalone server is better wherever you can run
it, but this lets you try Fourfold on hosting you already have.

### Setting it up

You need SSH access to the account.

1. **Put the `fourfold` program outside the web root**, e.g. in `~/bin/`. A
   shared host is almost always x86_64 Linux: use the static build
   (`tools/linux/build-static.sh` in the source; see
   [building.md](../building.md#a-linux-binary-for-shared-hosting)), which
   runs on any of them, whatever their C library. Make it executable
   (`chmod 755 ~/bin/fourfold`).
2. **Put the app folder outside the web root too**, e.g.
   `~/fourfold-apps/club/`. If Apache could reach it, it could serve your
   code, data and secrets as plain files, so Fourfold refuses to serve an app
   whose folder is inside the site's web root.
3. **Run `cgi-setup`** with the app folder and the site's web root (the
   folder the host serves the domain from):

   ```sh
   ~/bin/fourfold cgi-setup ~/fourfold-apps/club ~/club.example.com
   ```

   It makes the app folder's partitions if they're missing, and writes two
   files into the web root: `fourfold.cgi`, a two-line script that runs
   `fourfold` on the app, and an `.htaccess` that sends **every** request to
   it, static files included, so the app's access rules cover everything.
   Anything else in the web root is no longer served (it lists what it
   hid). If there's already an `.htaccess` it didn't write, it stops; move
   that one aside, or use `--force`.
4. **Upload the app's code** into `~/fourfold-apps/club/code/`, set
   `config.domain` in `private/config/app.ms`, and visit the site.
5. **If the app has background jobs or a schedule**, add a cron job (in the
   host's control panel, or `crontab -e`) that runs every minute:

   ```
   * * * * * ~/bin/fourfold run-jobs ~/fourfold-apps/club
   ```

The first request compiles the app, which takes a moment; later ones reuse
the compiled version (below).

**FastCGI.** If the host runs `mod_fcgid`, `cgi-setup --fastcgi` writes
`fourfold.fcgi` instead, run by `mod_fcgid`: one process serves many
requests, keeping the compiled app in memory, so requests are quicker.
Apache starts and stops these processes itself. If the site shows Apache's
own error page or the script's text instead, the host doesn't offer
`mod_fcgid` for `.fcgi` files; run `cgi-setup` again without `--fastcgi`.
(`fourfold fastcgi <folder> --listen HOST:PORT` serves FastCGI on a port,
for a server whose Apache uses `mod_proxy_fcgi`.)

### Deploying

The same as with the standalone server: upload changes to `code/`, then
`touch code/.deploy` (over SSH) to load them at once. Without `.deploy`,
the next request after the uploads have stopped for two seconds loads
them. A version that doesn't compile is never loaded: the site keeps
serving the previous one, and the errors go to the engine log.

Each request is served from a copy of the code its version compiled from
(`private/state/cgi/`), not from `code/` itself, so a request that arrives
mid-upload sees the old version or the new one, never a mix.

**Migrations run on their own:** the request that loads a version with new
migrations takes a backup, applies them, and only then switches to it.
Requests that arrive meanwhile wait (up to `deploy.migrationWaitSeconds`,
then get a 503). If a migration fails, nothing is changed and the previous
version keeps serving; the error is in the engine log. `fourfold migrate`
isn't needed.

### What's different

| | Standalone server | CGI / FastCGI |
|---|---|---|
| Live updates (`/live/`, `publish`) | yes | **no**: `/live/` isn't served and `publish` does nothing. Pages poll instead (`hx-trigger="every 5s"`) |
| Background jobs | run by the server | run when cron runs `fourfold run-jobs` |
| Loading new code | `touch code/.deploy` | the same, or two quiet seconds after an upload |
| Migrations | `fourfold migrate` | applied by the first request on the new version |
| OS-level protection | the engine can run as its own user, with `code/` read-only to it | **none**: under suEXEC, Fourfold runs as you, and could write anything you can. The partitions are kept by the engine only |
| HTTPS | from your proxy | from the host's Apache (turn on its certificate) |
| Limits | Fourfold's | Fourfold's, and the host's on top (process time, memory, processes) |

Every new version writes a line to the [engine log](#logs) saying so.

**Don't also run `fourfold serve` or `dev` on the same app folder.** CGI
requests are refused (503) while one is running, since their locks are in
memory where the CGI processes can't see them.

**Speed.** With CGI, each request starts a process and compiles its page's
code: about 10–15 ms for a small app on a fast machine, 30 ms for a
100-page one, plus whatever the page itself does. The first request after a
deploy compiles the whole app. FastCGI saves most of that.

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
have no `hosts` and no `verify`: those are for the app's own code (see
[Outbound requests and app mail](#outbound-requests-and-app-mail)), and
Fourfold won't use one secret for both. So the app's code can never see the
mail password.

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
server.ceilings.jobs = {"instructions": 100000000, "seconds": 60, "workers": 2}
server.ceilings.server = {"connections": 2000, "requests": 64}
```

| Ceiling | The app's setting, and its default |
|---|---|
| `budget.instructions` | instructions per request: 10,000,000 |
| `budget.seconds` | seconds per request, including waiting: 10 |
| `session.capKB` | the largest session: 64 KB |
| `session.anonIdleHours`, `anonMaxDays` | when an anonymous session ends: 24 hours idle, 7 days at most |
| `session.userIdleDays`, `userMaxDays` | when a signed-in session ends: 14 days idle, 30 at most |
| `session.anonPerAddress`, `anonTotal` | anonymous sessions per visitor address, and in all: 1,000; 100,000 |
| `jobs.instructions`, `jobs.seconds` | each background job run's limits: 100,000,000; 60 |
| `jobs.workers` | background jobs running at once: 2 |
| `server.connections`, `connectionsPerAddress` | connections at once, in all and per address (see [Connection limits](#connection-limits)) |
| `server.streams`, `streamsPerAddress` | live-update streams open at once, in all and per address: 1,000; 20 |
| `server.requests` | requests handled at once: 64 |

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

A deploy that brings new migrations compiles, but the old version keeps
serving until you apply them; see the next section.

---

## Migrations: `fourfold migrate`

An app's migrations (`code/migrations/`) change the shape of its data: new
tables and columns, reshaped data files. Fourfold applies them only when you
say so:

```sh
fourfold migrate /srv/club
```

If the app's server is running, the command asks it to apply them (through a
request file in `private/state/control/`) and waits. The server:

1. holds new requests (they wait, rather than fail), and lets those running
   finish;
2. takes a backup, `private/state/backups/<time>-before-migrations/`;
3. runs the pending migrations, in order, all their database work in one
   transaction;
4. switches to the new version of the code, and lets the held requests go
   on to it.

If a migration fails, nothing changes: the database work is undone, the data
files are put back from the backup, the old version goes on serving, and the
command prints the error (as does the engine log). If no server is running,
the command does all this itself, holding the app's lock so no server starts
meanwhile.

Requests that would wait longer than `deploy.migrationWaitSeconds` (30) get a
503 page instead. A migration may run for `deploy.migrationBudgetSeconds`
(600).

**A fresh install** has every migration pending, so `fourfold serve` won't
start until they're applied: run `fourfold migrate` once, or start with
`fourfold serve --migrate` (for scripted deploys).

**Code older than the data** (the database has migration 7, the code only
knows 1 to 5, say after deploying an old copy by mistake) won't run: the
deploy is refused, as if it didn't compile, and the old version keeps
serving. The fix is to deploy code that includes the migrations, or to
restore a backup from before them. In an emergency,
`fourfold serve --allow-older-code` (or `fourfold migrate --allow-older-code`)
runs it anyway, with a warning; the old code may then break on data it
doesn't understand.

**An applied migration that was edited** is refused the same way. Its author
should write a new migration instead.

**Sessions** keep whatever shape old code gave them. If that's a problem,
`config.deploy = {"clearAnonymousSessions": true}` signs out every anonymous
visitor after migrations run.

---

## A Postgres database

An app's database is SQLite unless its config names a Postgres server. The
app's code and queries are the same either way (a query or migration may
have a `.pg.sql` variant where the two differ).

```
// private/config/app.ms
config = {}
config.db = {"url": "postgres://fourfold@db.internal/club", "passwordSecret": "dbPassword"}
```

```
// private/config/secrets.ms
secrets = {}
secrets.dbPassword = {"value": "..."}
```

- **Give each app its own database (or schema) and its own role**, with
  rights to that database only. `config.db.schema` makes the app work in one
  schema (`search_path`), for apps sharing a database.
- **The password goes in `secrets.ms`**, never in the URL: a URL with a
  password in it is an error. The app's code never sees it.
- `db.pool` (4) is how many connections the app keeps; `db.queryTimeoutSeconds`
  (the request budget, 10) how long one query may run.
- Changing `config.db` takes a restart.
- **Back up a Postgres database with its own tools** (`pg_dump`, or your
  provider's backups): Fourfold's backups have the app's files, users and
  sessions, but not a Postgres database. Before each `fourfold migrate`,
  take one yourself. If a migration fails, its transaction undoes it.
- Several servers sharing one database coordinate migrations themselves:
  one migrates while the others wait, then finds the work done.
- An app moving from SQLite to Postgres starts with an empty database: its
  migrations make the tables, but copying the rows over is up to you, for
  now.

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
| `access` | a request refused by the site's access rules |
| `limits` | a request over a rate limit, with the limit |
| `spam` | a form sent too soon after it was shown, without its timing token, or without a solved puzzle |
| `session` | session problems, like one too big to save |
| `auth` | sign-in links sent, refused, throttled or failed to send; sign-ins and sign-outs; new users |
| `mail` | the whole message, when `mail.transport` is `"log"`; app mail (`mail.send`) sent, refused or failed |
| `outbound` | each request the app's code sent to another server: method, URL (without its query string or secrets), status, time and size; or why it was refused |
| `migrate` | migrations applied, or why they failed |
| `backup` | scheduled and requested backups |
| `tls` | certificates asked for, received, or refused (with the CA's reason) |
| `csp` | a browser's report of something the Content Security Policy blocked: the directive, what was blocked, the page |
| `stop` | the server shutting down |
| `jobs` | each background job run, with its time; failures, retries and dead jobs; enqueued jobs dropped because their request failed |

Control characters in a message are escaped, so nothing logged can fake a
line. The app can't write to this log. Files older than 30 days are
removed (checked hourly); `config.logging = {"keepDays": 90}` keeps more.

**The app's output**, in `dropbox/`: what its code `print`s goes to
`dropbox/print.log`, and the app may write reports there. The site's author
may want to see these.

---

## Backups

```sh
fourfold backup /srv/club
```

takes a backup of the app: everything in `data/` (with its SQLite database),
and `private/state/` (users, roles, sessions, the CSRF key), but not the
engine logs. If the server is running, it asks the server, which holds
requests for the moment it takes; SQLite files are copied with SQLite's own
backup, which is consistent even while they're written. Each backup is a
folder, named for its time and reason:

```
private/state/backups/
  20261105-140212-manual/
    backup.json      when, why, and which migrations had run
    data/
    state/
```

Fourfold also takes one **before every batch of migrations**, and, if
`config.backups.everyHours` is set, **on a schedule**. Of these automatic
ones, it keeps the newest `backups.keep` (10); the ones you take, and those
taken before a restore, it never removes.

**Keep backups somewhere else too.** `private/state/backups/` is on the same
disk as the app. `config.backups.folder` can put them elsewhere (a full path;
it can't be inside `code/`, `data/`, `dropbox/` or `private/config/`, since a
backup holds users and sessions). Either way, copy them off the machine
regularly, and keep them as private as `private/state/` itself.

**To restore**, stop the server, then:

```sh
fourfold restore /srv/club --list                 # the backups, newest first
fourfold restore /srv/club 20261105-140212-manual
```

It first backs up what's there now (`...-before-restore`), so a restore can
be undone too, then replaces `data/` and `private/state/` with the backup's.
`--data-only` restores `data/` alone, keeping today's users and sessions.
Restoring an old backup signs out everyone who signed in since, and takes
the data back to the migrations of that time: deploy matching code.

What a backup doesn't have: `code/` (the author's, usually kept elsewhere
too), `private/config/` (settings and `secrets.ms`: back them up yourself,
as privately), and a Postgres database (use `pg_dump`).

---

## Known gaps

`fourfold serve` prints the first when it starts.

- **No per-request memory limit.** Time and instruction limits apply, but a
  script can use as much memory as the machine has. A memory limit for the
  process from the service manager (`MemoryMax=` in systemd) at least
  protects everything else on the machine.
- **Visitor addresses behind a proxy** are known only if you list the
  proxy in `config.proxy.trust`; see
  [Behind a reverse proxy](#behind-a-reverse-proxy).
- **One app per server on ports 80 and 443.** Several apps on one machine
  each need their own ports behind a reverse proxy, until `fourfold front`
  arrives (M11.1). Likewise, running each app as its own OS user is up to
  you for now.
- **Certificates by HTTP-01 only:** no wildcard certificates from Let's
  Encrypt (give one as files instead).
- **CGI and FastCGI hosting** has no live updates, and no protection from
  the OS (see [Shared hosting](#shared-hosting-cgi-and-fastcgi)).
- **Dead background jobs** can be seen and retried only in dev mode's jobs
  panel; on a server, they're in the engine log (a production console is
  planned).
- **Live updates go to one process.** Fourfold runs as one process per app,
  so this matters only if you run several behind a load balancer (not
  supported yet).
- **Moving an app from SQLite to Postgres** gives it an empty database:
  copying its rows over is up to you.

---

## Quick reference

### Commands

| Command | Does |
|---|---|
| `fourfold serve <folder> [options]` | serve an app (see [Running](#running-fourfold-serve)) |
| `fourfold dev <folder> [--port N]` | serve an app on `localhost`, for its author |
| `fourfold migrate <folder>` | apply the app's pending migrations (see [Migrations](#migrations-fourfold-migrate)) |
| `fourfold backup <folder>` | back up the app (see [Backups](#backups)) |
| `fourfold restore <folder> [NAME] [--data-only]` | list the backups, or restore one, with the server stopped |
| `fourfold cgi-setup <folder> <web-root> [--fastcgi]` | serve an app through the host's Apache (see [Shared hosting](#shared-hosting-cgi-and-fastcgi)) |
| `fourfold run-jobs <folder> [--max-seconds N]` | run due jobs, from cron, for an app served through CGI |
| `fourfold cgi <folder>`, `fastcgi <folder> [--listen HOST:PORT]` | what the web root's entry point runs |
| `fourfold version` | print the version |
| `fourfold help` | list the commands |

### Files

| File | Is |
|---|---|
| `code/.deploy` | touch it to load changed code |
| `private/config/app.ms` | the app's settings (the `config` map) |
| `private/config/secrets.ms` | passwords and tokens (the `secrets` map) |
| `private/state/state.db` | sessions, users, roles, sign-in links, rate-limit counters, the job queue and the schedule's state |
| `private/state/secret.key` | signs CSRF tokens, form timestamps and puzzles; made if missing |
| `private/state/logs/` | the engine log |
| `private/state/backups/` | backups |
| `private/state/tls/` | certificates from Let's Encrypt, their keys, and the ACME account |
| `private/state/cgi/` | CGI mode's compiled versions, each with a copy of its code (made again as needed) |
| `data/.fourfold/app.sqlite` | the app's SQLite database |
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
| `proxy.trust` | `[]` | your reverse proxy's addresses (see [Behind a reverse proxy](#behind-a-reverse-proxy)) |
| `limits.anonymous`, `limits.signedIn` | `"60/minute"`, `"300/minute"` | default rate limits (see [Rate limits](#rate-limits)) |
| `outbound.enabled`, `allow`, `allowAddresses` | on, no cap, none | the app's outbound requests (see [Outbound requests and app mail](#outbound-requests-and-app-mail)) |
| `outbound.perMinute`, `maxResponseKB`, `timeoutSeconds` | 120, 5120, 10 | their limits |
| `mail.appMail`, `appPerHour` | on, 100 | the app's own mail (`mail.send`) |
| `jobs.timeZone`, `workers`, `seconds`, `maxAttempts` | `"UTC"`, 2, 60, 5 | background jobs (see [Background jobs and live updates](#background-jobs-and-live-updates)) |
| `db.url`, `passwordSecret`, `schema`, `pool` | SQLite | a Postgres database (see [A Postgres database](#a-postgres-database)) |
| `deploy.migrationWaitSeconds`, `migrationBudgetSeconds` | 30, 600 | how long requests wait for migrations; how long one may run |
| `deploy.clearAnonymousSessions` | `false` | sign out anonymous visitors after migrations |
| `backups.folder`, `everyHours`, `keep` | `private/state/backups/`, none, 10 | where backups go, how often, how many automatic ones to keep |
| `tls.email`, `acme` | none, Let's Encrypt | with `--tls`: the ACME account's contact; the CA (`"letsencrypt-staging"`, or a directory URL) |
| `tls.certFile`, `keyFile` | none | with `--tls`: a certificate of your own, in `private/config/` |
| `tls.http2` | `true` | HTTP/2 over TLS |
| `server.*` | see [Connection limits](#connection-limits) | connection and request limits |
| `logging.keepDays` | 30 | how long engine log files are kept |

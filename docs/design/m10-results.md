# Fourfold: M10 Results — CGI and FastCGI hosting

*Status: 2026-11-12, at the end of M10 ([milestones.md](milestones.md)). Records what was built for serving an app through a web server that isn't Fourfold (shared hosting's Apache), the choices made, and how it was tested, including on a real shared-hosting account. Builds on the M5 spike ([m5-results.md](m5-results.md), R8). No MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| Is a CGI request fast enough? | **Yes.** About 10–15 ms for a small app (release build, on the x86_64 Mac used for development), 33 ms for a 100-page one, against 180 ms for the latter in the M5 spike, which compiled everything on every request. The first request after a deploy compiles the whole app (300 ms for 100 pages). |
| Does a broken or half-finished upload take the site down? | **No.** Each version serves from a snapshot of the code it compiled from; a version that doesn't compile is never installed. |
| Do sessions and data stay exact across processes? | **Yes.** 400 requests from 16 concurrent CGI processes on one session counted exactly, with a deploy in the middle; 80 concurrent requests through Apache likewise. |
| Do migrations work without `fourfold migrate`? | **Yes.** The request that installs a version applies its migrations first, under a cross-process schema lock, with a backup; one that fails leaves the old version serving. |
| Do jobs run? | **Yes**, from cron: `fourfold run-jobs`. |
| Does it work under real Apache? | **Yes**: macOS's Apache (CGI, and FastCGI through `mod_proxy_fcgi`), and Debian's with suEXEC (CGI, and `mod_fcgid`), each request running as the site's user. |
| Does it run on any shared host's Linux? | A fully static x86_64 build (musl) passes the whole test suite on Linux, and ran on DreamHost (Ubuntu 24.04) once it looked for the CA certificates where that system keeps them. |
| Does it work on real shared hosting? | **Yes**: newsfeed on DreamHost, through its Apache (CGI, suEXEC, HTTPS), with `run-jobs` from cron fetching stories over HTTPS. |

---

## What was built

- **`hosts/cgi`** (new component):
  - `CodeCache` (`code_cache.h`): CGI mode's compiled app between requests. A version is `private/state/cgi/v<N>/`: a snapshot of `code/` (`code/`) and `version.json` (every `.ms` file's handlers, and which `.ms` file each route's code is in); `cgi/state.json` says which version serves and what's pending. Each request lists `code/` and compares a fingerprint (paths, sizes, times, the `.deploy` time, the database dialect). A change is recorded as pending; a request that finds the same change pending for the quiet period (2 s) compiles it, as does one that sees `.deploy` touched, or finds no version at all. One process compiles at a time (a lock file); the others keep serving the current version, unless there's none yet. A failed compile's key is remembered, so it isn't tried on every request. Replaced versions are removed ten minutes after they're replaced.
  - `CgiApp` (`cgi_app.h`): the app's services (config, `state.db`'s stores, the database, data files, file-lock session locks), and `handle(request, https)`: the version from `CodeCache`, an engine for it, the schema lock held shared, the request served. Migrations at install (`beforeInstall`): plan, then with the schema lock exclusive, backup, apply, restore on failure. FastCGI keeps the full version and its engine in memory (`keepVersion`). It logs a version's installation and what CGI mode lacks (`kCgiModeNotice`), and refuses requests (503) while `fourfold serve` or `dev` holds the app's server lock.
  - `SchemaFileLock` and `FileSchemaGate` (`schema_lock.h`): the schema lock between processes, as two lock files: requests take `schema-gate` then `schema` shared; migrating takes both exclusive (the gate first, so a busy site can't starve it).
  - `cgi_request.h`: CGI variables ↔ `HttpRequest`, and the CGI response. Every `HTTP_*` variable is a header; `REDIRECT_HTTP_AUTHORIZATION` too; body limits as the standalone server's (1 MB, 33 MB for multipart), refused with 413 before reading.
  - `fastcgi.h`: FastCGI 1.0, the responder role: records, name-value pairs, `RecordReader`, and `serveConnection` (one request at a time per connection, `FCGI_KEEP_CONN`, `FCGI_GET_VALUES`, abort, unknown roles, multiplexing refused, bodies over their `CONTENT_LENGTH` or the limits refused with 413).
- **`hosts/cli`**: `fourfold cgi <folder>` (the spike, replaced: `CgiApp` per request, and the web root check against `DOCUMENT_ROOT`); `fourfold fastcgi <folder> [--listen PATH|HOST:PORT]` (fd 0 as `mod_fcgid` gives it, or a socket; SIGTERM, SIGINT and SIGHUP stop it between connections); `fourfold cgi-setup <folder> <web-root> [--fastcgi] [--binary PATH] [--force]`; `fourfold run-jobs <folder> [--max-seconds N]` (the job runner with `FileSchemaGate`, until nothing is due, one run at a time, nothing if a server runs the app); `migrate` and `restore` hold the schema lock exclusively when they work offline, so CGI requests wait.
- **`compiler`**: `CompileOptions::knownHandlers` and `compileOnly`: a partial compile takes every file's handlers as known and compiles only the given files (none at all for a route without code); `CompileOutput::handlers`. The "defines no handlers" warning counts webhooks.
- **`script`**: `ScriptSources::only`: compiles only those page and job files and the libs they reach through `import` (all libs if one imports by something other than a literal); no migrations or seed.
- **`partition`**: `StateDir::saveCodeSnapshot`, `openCodeSnapshot` (a `CodeTree` over a snapshot), `list`, `removeTree`; `FileLock` shared mode and `acquireWithin`; `writable::copyFileInto` (keeping modification times) and `removeTree`.
- **`platform`**: `caBundleFallback` (`ca_bundle.h`): when the TLS library's built-in CA file isn't there (the static build carries Alpine's `/etc/ssl/cert.pem`), `$SSL_CERT_FILE` or the first of the usual places (Debian and Ubuntu's `/etc/ssl/certs/ca-certificates.crt`, Red Hat's, SUSE's); outbound HTTP and mail set it with `CURLOPT_CAINFO`.
- **`pipeline`**: `EngineOptions::publishWithoutPush`; `SchemaGate::enter` and `leave` are virtual.
- **`appdata`**: backups leave out `private/state/cgi/` (and a restore leaves it alone).
- **`devtools`**: the jobs panel says how jobs and channels behave under CGI.
- **Sample:** `examples/newsfeed` has `/poll`, the stories list fetched every 30 s, for hosting without SSE.
- **Tests and tools:** `tests/apache/apache_test.sh` (Apache in four setups, below); `tools/linux/build-static.sh` and `suexec-test.sh` (Docker).

## Choices made here

- **Decided with the user** (milestones.md): FastCGI on fd 0 as `mod_fcgid` starts it, and on `--listen` for `mod_proxy_fcgi`; FastCGI keeps the version in memory but runs no jobs; the entry point is a shell script in the web root that `exec`s the binary kept outside it; the shared-hosting binary is fully static.
- **What's cached is handlers, not templates.** Measured on a 100-page app: running every `.ms` file's top level was 150 of 172 ms; templates and Markdown 15 ms. So a version keeps the handlers (as JSON), and a request compiles everything else again from the snapshot, which needs no serialization of `CompiledApp` (and so no format to keep in step with it). Serializing templates too would save most of the remaining ~15 ms per 100 pages, if that ever matters.
- **Versions serve from a snapshot of `code/`.** Without one, CGI couldn't keep §8's promise that a broken or partial upload never takes the site down: a request compiles its page's code when it arrives, and the files may be mid-upload. Copying is cheap next to a full compile, happens once per deploy, and keeps modification times (for caching headers).
- **Wait for quiet across requests.** A process can't wait two seconds, and rsync gives files their original times, so "quiet" is a change that a later request finds unchanged two seconds after an earlier one first saw it. Touching `code/.deploy` skips the wait, as in serve mode.
- **Migrations run at install, not on demand.** Shared hosting has no long-running process to ask; the version waits, compiled, until its migrations are applied, and the request that installs it applies them. That request waits for those in flight (the exclusive schema lock); requests arriving meanwhile wait up to `deploy.migrationWaitSeconds`, then get a 503.
- **`publish` succeeds and goes nowhere** in CGI mode, rather than failing: nothing can subscribe there, so there's no one to tell, and the same app (newsfeed's job, say) runs on either kind of hosting. `/live/` isn't served.
- **`run-jobs` holds the schema lock a job at a time** (`FileSchemaGate` over the job runner's gate, now virtual), so a deploy's migrations wait only for jobs actually running. It stops when nothing is running or due, or after `--max-seconds` (50, so cron's next run doesn't find it still going; a job still running then is abandoned and runs again next time).
- **`cgi-setup` sends everything to Fourfold**, static files included (so access rules cover them), with `Options -Indexes`, `DirectoryIndex disabled`, and a rewrite to the entry point. `CGIPassAuth On` (Apache 2.4.13+) passes `Authorization` to webhooks: suEXEC drops the `REDIRECT_` variables a rewrite would leave it in. It refuses an app folder (or the binary) inside the web root, and someone else's `.htaccess`.
- **The web root is checked on every request** too (`DOCUMENT_ROOT`): an app folder inside it, the app folder as the web root, or a web root inside a partition is refused with a 500.
- **No server lock in CGI mode** (many processes at once): instead a request checks that no `fourfold serve` or `dev` holds it, and refuses (503) if one does.

## Tests

- `test_cgi` (new): `CodeCache` (first compile, partial serving, quiet period, failed versions not retried, `.deploy`, an app that has never compiled, retiring old versions, handlers round-trip); `CgiApp` (sessions across "processes", migrations at install and a failed one keeping the old version, the 503 while another process migrates, `/live/` not served, `publish` going nowhere); CGI variables; FastCGI records, pairs, connections, limits, roles, multiplexing, management records, abort.
- `test_compiler`: a partial compile describes the same app as a full one, never touches an unimported lib (a broken one), and compiles no code for a route without any.
- `test_partition`: code snapshots (times kept, links and dot-files not copied, later changes not seen, replaced, removed).
- `test_fourfold`: `fourfold cgi` end to end (and refusing an app inside the web root, and a 2 MB body); `webRootProblem`; `run-jobs` running what CGI requests enqueued and what's scheduled; `cgi-setup` (entry point, permissions, `.htaccess`, switching to FastCGI, someone else's `.htaccess`, an app inside the web root).
- **Under real Apache** (`tests/apache/apache_test.sh`, 26–28 checks per setup: pages, static files through Fourfold, Markdown, 404, the entry point and `.htaccess` not served, sessions, a form post with CSRF and one without, a webhook's `Authorization`, a job queued then run by `run-jobs`, a deploy, a broken deploy, 80 concurrent requests on one session):
  - macOS's Apache 2.4.56, CGI: all pass.
  - macOS's Apache, FastCGI through `mod_proxy_fcgi`: all pass.
  - Debian's Apache 2.4 with suEXEC (`apache2-suexec-custom`), CGI, run as root in a container with the static build (`tools/linux/suexec-test.sh`): all pass, and everything in the app folder belongs to the site's user.
  - The same with `mod_fcgid`: all pass, with `fourfold fastcgi` processes kept by `mod_fcgid` and running as the site's user.
- **The M9 samples under Apache CGI**: weather's pages; newsfeed's `/poll`, with `run-jobs` fetching Hacker News's stories into it.
- **The static Linux build** passes the whole suite on Alpine (musl). Getting there fixed two portability problems in older tests (doctest needed `DOCTEST_CONFIG_USE_STD_HEADERS` for libstdc++; a missing `<atomic>`).
- **On DreamHost** (the user's account: Ubuntu 24.04.4, glibc 2.39, x86_64; SSH): the static binary in `~/bin/`, newsfeed in `~/fourfold-apps/`, `cgi-setup` into a new subdomain's web root, HTTPS from DreamHost's Let's Encrypt certificate; `run-jobs` every minute from cron. Pages and `/poll` served through CGI. The first try found a real bug: every outbound HTTPS request failed ("Problem with the SSL CA cert"), since the static libcurl looked for Alpine's CA file, which Ubuntu doesn't have; fixed by `caBundleFallback`, checked in a bare Ubuntu 24.04 container, then on DreamHost, where the job then ran on schedule. Whether DreamHost offers `mod_fcgid` wasn't tried (the test site was taken down first).
- **Across processes**: 16 concurrent loops of `fourfold cgi`, 25 requests each on one session, with a deploy partway: all 400 answered 200, the session counted exactly.

## Still to do

- **A production admin console** for CGI-hosted apps: dead jobs and the migration state are in the engine log only.
- **Caching templates in a version** too, if apps much larger than 100 pages are served through CGI.
- **The static build has no Postgres** (libpq doesn't link statically on Alpine without more work); shared hosts rarely offer Postgres.
- **Several FastCGI processes share nothing in memory**: each compiles the version on its first request after a deploy.
- **Unifying `CgiApp` and `AppServer`'s service setup**: both open the same stores in the same way; a shared piece would keep them from drifting.

## Still to check

- Whether DreamHost (and other common hosts) offer `mod_fcgid` for `.fcgi` files, so `cgi-setup --fastcgi` works there.
- The weather sample on real hosting (it ran under local Apache CGI); its mail needs an SMTP or Postmark account in the config.

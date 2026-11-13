# Fourfold: M11 Results — Production server

*Status: 2026-11-13, M11 built and running on a test VPS (`test.miniscript.org`): external TLS and header scans A+, slowloris and a connection flood from outside contained; the load test and watching renewal over days are deferred, with the user ([milestones.md](milestones.md)). Records what was built for one app facing the internet on its own, the choices made, and how it was tested. No MS2 changes; R4 deferred again (below).*

## Summary

| Question | Outcome |
|---|---|
| Can Fourfold end TLS itself? | **Yes.** OpenSSL 3, TLS 1.2 and 1.3 with forward-secret AEAD ciphers only; certificates chosen by SNI and replaced while serving. |
| HTTP/2? | **Yes**, by ALPN, through nghttp2: concurrent streams, uploads under the same limits, server-sent events as streams, GOAWAY on shutdown. |
| Certificates without setup? | **Yes.** Our own ACME client (RFC 8555, HTTP-01) got a certificate from Pebble (Let's Encrypt's test CA) in Docker and served it over HTTP/2, with Pebble rejecting 5% of nonces. Stored in `private/state/tls/`, renewed with 30 days or a third of their life left. Or PEM files of the app's own. |
| Slow clients? | **Contained.** 10 s for headers (the TLS handshake included), then 408; bodies at 1 KB/s on average after 10 s; 100 connections per address. Locally, 150 slowloris connections over TLS from one address: 100 held, 50 refused, all 100 given a 408 at 11 s. |
| Ports 80 and 443 without root? | **Yes**, by systemd's socket activation (tested in a forked child as systemd does it), or `CAP_NET_BIND_SERVICE`. |
| Graceful shutdown? | **Yes**: SIGTERM stops accepting, closes idle connections, ends streams, finishes requests (30 s at most). |
| Per-request memory (R4)? | **Deferred again**, with the user: MS2 has no per-VM accounting yet; `MemoryMax=` is the guard, and the startup notice says so. |

---

## What was built

- **`tls`** (new component, OpenSSL): `certificates.h`: P-256 keys, self-signed certificates, CSRs (DER), inspecting a chain (names, dates, issuer), checking a key matches, `hostMatches` (one-label wildcards), and for ACME, `publicJwk` (canonical, for the thumbprint) and `signEs256` (raw r‖s). `server_tls.h`: `ServerTls`, the certificates by name (exact before wildcard; the first for no SNI), each in its own `SSL_CTX`, swapped in by the SNI callback; `install` is thread-safe and replaces by first name. `TlsSession`: one connection's TLS over memory BIOs, so the connection does all socket I/O itself.
- **`acme`** (new component, libcurl): `AcmeClient`: directory, nonces (badNonce retried), account (found or registered; registered again if the CA doesn't know the stored one), order, HTTP-01 on a `ChallengeBoard`, polling, finalize with a CSR, download, and a check that the certificate is for our key. `CertificateManager`: on its own thread, installs what's stored, renews when needed, writes key then certificate (0600), backs off after failures (1 min, 10 min, 1 h, 6 h), keeps the old certificate while it can't renew. Only public DNS names (no IPs, `localhost`, single labels).
- **`httpserver`**: connections rebuilt around a wire buffer, with TLS through `TlsSession`; HTTP/2 (`http2.cpp`, nghttp2, for both its old and new APIs); limits: header timeout (covering the handshake and a connection's first request), minimum body rate after a grace period, send timeout restarted by progress, per-address connection caps (with exempt ranges), stream caps total and per visitor address (503 with `Retry-After`); `adopt` for inherited sockets, several listeners, `shutdown(deadline, done)`; `limitConcurrency` (requests at once, a queue with a wait, then 503).
- **`platform`**: `takeSystemdSockets` (`LISTEN_PID`/`FDS`/`FDNAMES`); `[::]` listeners are dual-stack.
- **`config`**: `config.tls` (`acme`, `email`, `acmeCaBundle`, `certFile`/`keyFile`, `http2`), `config.headers` (`csp` sources by kind, `strictStyles`, `frameAncestors`, `hsts`, `permissions`), `config.server` (connection and request limits, under the server config's new `server` ceilings), `config.logging.keepDays`. `HeadersConfig` holds the headers ready to send, the CSP split at the nonce.
- **`pipeline`**: headers from the config; HSTS in production over HTTPS; `/__fourfold/csp-report` (report-uri's and the Reporting API's formats), logged at most 30 a minute and passed to the observer; `Forwarded` read ahead of `X-Forwarded-For`.
- **`state`**: `EngineLog::removeOldFiles` (retention).
- **`devtools`**: the page script shows CSP violations (`securitypolicyviolation`) with the template warnings.
- **`cli`**: `fourfold serve --tls` (`[::]:443`, and `[::]:80` redirecting to one of the app's names and answering challenges; `--redirect-listen`, `--no-redirect`); systemd sockets by name or order; SIGTERM and SIGINT drain (a second stops at once), in `dev` too; limits from `config.server`, with loopback exempt from per-address caps; open-file limit raised; log retention hourly.
- **Tools:** `tools/acme/pebble_test.sh` (Pebble in Docker; the test domain is `host.docker.internal`, which Docker's own DNS gives Pebble); `build-static.sh` links OpenSSL and nghttp2 statically.

## Choices made here

- **Decided with the user** (milestones.md): OpenSSL 3 and nghttp2; R4 deferred again; M11 split into M11 (one app) and M11.1 (several apps, OS users, `front`, multi-process); a VPS from the user for the exit criteria, Pebble until then.
- **TLS over memory BIOs**, not on the socket: the connection keeps one read path and one write path, TLS never "wants to write while reading", and HTTP/2 sits on the same plaintext.
- **No session resumption** (no tickets, no cache): simpler, safe with SNI switching contexts, and with keep-alive and HTTP/2 a visitor makes few handshakes.
- **The header timeout starts with the connection**, so a TLS handshake that stalls, or a connection that never sends, is a slowloris like any other (10 s, then closed or 408).
- **Body speed is an average after a grace period** (1 KB/s after 10 s), checked each second, not an absolute time: a 33 MB upload on a slow line still works, a body trickled to hold the connection doesn't.
- **Admission counts requests, not VMs**: a wrapper around the handler, at the transport, so static files and pages alike queue (briefly) and get a 503 under overload; the script host is unchanged.
- **Stream caps are per visitor address** (the forwarded one, behind a trusted proxy), separate from connections, and refused with a 503 the SSE client retries.
- **Loopback is exempt from per-address caps**: a local proxy not listed in `proxy.trust` would otherwise look like one visitor with every connection, and nothing outside can connect from loopback. Totals still apply.
- **Early HTTP/2 refusals wait for nghttp2**: submitting a response inside the HEADERS callback made nghttp2 close the session; refusals (413, 431, 400) are queued and sent once the input is processed. A client that declares a huge length and sends little still loses its connection (nghttp2's content-length check), which is fine.
- **Renew at a third of the lifetime** (or 30 days, whichever is sooner): Pebble, and Let's Encrypt's short-lived profile, issue 6-day certificates, which a fixed 30 days would renew on every check.
- **Port 80 redirects to the app's own names only**: the `Host` is used only to choose among them, never copied into `Location`.
- **CSP reports**: both `report-uri` (Firefox) and `report-to` with `Reporting-Endpoints` (Chromium); dev mode also catches violations in the page itself, where the element is known, rather than waiting for the report.
- **Added CSP sources are validated**: hosts (with optional scheme, wildcard label, port, path) or a few schemes; no keywords (so never `'unsafe-inline'` or `'unsafe-eval'` for scripts), no `*`, no bare scheme for scripts.

## Tests

- `test_tls` (new): keys, self-signed certificates and their contents, key mismatch, CSRs, wildcard matching; handshakes over memory BIOs with an OpenSSL client: TLS 1.3, ALPN (h2, http/1.1, none, HTTP/2 off), TLS 1.1 refused, SNI choosing exact before wildcard, the first for no SNI, replacing a certificate, garbage failing.
- `test_acme` (new): ES256 and the JWK; against a fake CA that verifies every JWS signature and nonce and checks HTTP-01 answers on the board: the whole flow, the account reused, a failed challenge's reason, badNonce retried, an unknown account registered again; `CertificateManager`: obtain, files and permissions, nothing asked when fresh, a restart installing the stored one, renewal by lifetime, new names, a failure keeping the old one; only public names.
- `test_httpserver`: the existing HTTP/1.1 tests (timeouts renamed), plus TLS (HTTP/1.1, plain HTTP refused, a stalled handshake), HTTP/2 (a GET and the request as the handler sees it, cookies joined, concurrent streams answered out of order, POST bodies, 413 for a declared or actual oversize with the connection going on, HEAD, connection headers dropped), HTTP/2 off, SSE over HTTP/2 with its cap, connection caps total and per address with exemptions, stream caps per visitor address, exempt addresses, slow bodies, graceful shutdown (idle closed, busy finished, streams ended, the deadline, GOAWAY), `limitConcurrency`.
- `test_config`: `config.tls`, `config.headers` (the default CSP exactly; additions, strict styles, frame ancestors, HSTS, permissions; ten kinds of bad source refused), `config.server` within ceilings, `config.logging`.
- `test_pipeline`: `Forwarded`: quoted IPv6 with a port, IPv4 with a port, several hops, `unknown` and obfuscated names, precedence over `X-Forwarded-For`.
- `test_platform`: `takeSystemdSockets` refusing another process's, and taking two named sockets in a forked child.
- `test_state`: log retention.
- `test_fourfold`: `serve --tls` end to end over real TCP: ALPN h2, the production headers and nonce, another host refused, a CSP report logged; port 80's redirects (the bound port, the app's name not the visitor's, 308 for POST, an absolute target refused, an unknown challenge); ACME failing politely with port 80 still up; missing domain or certificate file refused at start; graceful shutdown.
- **By hand:** `fourfold serve --tls` with curl (HTTP/2 and HTTP/1.1, headers, `__Host-` cookies, the redirect); 150-connection slowloris over TLS (above); SIGTERM.
- **Pebble** (`tools/acme/pebble_test.sh`): a real certificate, then HTTPS over HTTP/2 with HSTS, trusting Pebble's root; with Pebble's 5% nonce rejection.
- **The static Linux build** (musl, now with OpenSSL and nghttp2 linked in; 15 MB) passes the whole suite on Alpine, epoll included. The first run found the new tests' OpenSSL clients dying of SIGPIPE on Linux (OpenSSL's socket BIO writes with `write()`); the tests now ignore it, and `serve` and `dev` ignore it too, as a safeguard.
- Everything also passes under ASan (`script`'s "requests are served while a long compile runs" timed out once there, in a component M11 didn't touch, and passed on a rerun).

## Still to do (M11.1 and later)

- `fourfold front`, an OS user per app, partition permissions, the append-only dropbox, several engine processes per app (if load testing calls for it).
- DNS-01 for wildcard certificates; OCSP isn't needed (Let's Encrypt has dropped it).
- `config.tls`'s files are read at startup only; reloading them on change would save a restart.

## On a public server

A Vultr VPS (1 vCPU, 1 GB, Ubuntu 26.10), `test.miniscript.org`, deployed by `tools/vps/deploy.sh`: the static build, the guestbook sample in `/srv/guestbook` owned by an unprivileged `fourfold` user, systemd opening ports 443 and 80 (socket activation) for `fourfold serve --tls`, `MemoryMax=700M`, `ProtectSystem=strict`, ufw opening 80 and 443.

- **Certificates:** one from Let's Encrypt's staging CA, then (config line and `private/state/tls/` removed, restarted) a real one, each within seconds of starting. Port 80 redirects with a 301.
- **SSL Labs: A+** (also A+ ignoring trust; no warnings). TLS 1.2 and 1.3 only; three ECDHE-ECDSA AEAD suites for 1.2, the three 1.3 suites; forward secrecy with every client; HSTS; ALPN h2 and http/1.1; no session resumption, no 0-RTT; every vulnerability check negative. Of 68 simulated clients, the 22 that fail are those Mozilla's intermediate profile gives up on (IE 6 to 10, Android up to 4.3, Safari up to 8, Java 6 and 7, OpenSSL 0.9.8, Chrome 49 on XP).
- **Mozilla Observatory: A+** (125; 12 of 12 tests).
- **Slowloris from outside** (a Mac on a home connection, over the internet): 150 connections over TLS sending a header line a second, each answered with a 408 ten seconds after it opened; 150 that never start the TLS handshake, each closed at ten seconds. Meanwhile a visitor from another address got the page in 0.1 s.
- **Connection flood from one address:** 300 TLS connections opened at once (0.5 s): 100 held (the server counted 100 established), 200 refused; the 100 dropped by the header timeout; another visitor got the page in 0.05 s meanwhile.
- **The admin guide's `--tls` example** set `config.domain` without `config = {}` first, which is an error; found deploying, fixed.

## Deferred, with the user

- A load test against an agreed target (it went with M11.1's question of several engine processes per app; both were dropped there, see [future.md](future.md)). The default anonymous rate limit (60 a minute per address) must be raised for a generator from one address.
- Renewal over days: Let's Encrypt's default certificates last 90 days, so renewal won't happen on its own for about two months; it's tested with Pebble's 6-day certificates. Asking for Let's Encrypt's short-lived profile (6 days) needs ACME profile support, which the client doesn't have yet.

# Fourfold: Future Features and Possible Enhancements

*Status: started 2026-11-13. Things deliberately left out of a milestone, kept here so they aren't lost. Each entry says where it came up and what it would take. Nothing here is a commitment; [milestones.md](milestones.md) is the plan.*

## Scaling

### Several engine processes per app

*Deferred from M11.1, with the user (2026-11-13).*

Run several engine processes for one app, so it can use more than one core. All state between requests already lives in SQLite or files (for CGI mode's sake), so most of the engine works unchanged; see [architecture.md](architecture.md), "Using more than one core".

- **Sharing the work:** `SO_REUSEPORT` on one socket, or one socket per process with `front` balancing across them.
- **SSE fan-out:** the real work. A `publish` in one process has to reach streams held by its siblings, e.g. a Unix-socket broadcast among them.
- **Memory:** each process holds its own compiled app, so a small VPS gains little; this pays off on boxes with several cores.
- The alternative remains a per-thread GC in MS2 (ms-source-changes.md), giving several script threads in one process.

## Logs

### Rotating dropbox files

*Deferred from M11.1, with the user (2026-11-13).*

Nothing rotates the files an app writes to `dropbox/` (including `print.log`), so they grow without limit. From M11.1 the per-app dropbox writer owns those files, so it's the natural place to rotate them (by size or date, keeping a set number), and the engine never needs read access. The engine's own logs are already rotated and pruned (`config.logging.keepDays`, M11).

### Rate-limiting the dropbox writer

*From M11.1.* The writer caps each write (1 MB) but not how much an app writes over time, so a runaway app can fill the disk through its dropbox. The writer is the natural place for a per-minute byte budget (the overview's reason for a separate logger process), along with rotation.

## TLS and certificates

### DNS-01 challenges and wildcard certificates

*From M11 ([m11-results.md](m11-results.md)).* Only HTTP-01 is supported, so every name needs its own entry in the certificate and must resolve to the server. DNS-01 needs an API client per DNS provider.

### Reloading `config.tls` files on change

*From M11.* PEM files named in `config.tls` are read at startup only; replacing them needs a restart.

### ACME certificate profiles

*From M11.* The ACME client doesn't ask for a profile, so Let's Encrypt issues its default 90-day certificates. Supporting profiles would allow its 6-day certificates, and would also make renewal on a real server observable within days.

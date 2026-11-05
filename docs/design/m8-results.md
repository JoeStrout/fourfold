# Fourfold: M8 Results — Databases and migrations

*Status: 2026-11-05, at the end of M8 ([milestones.md](milestones.md)). Records what was built for databases, named queries, migrations and backups, the choices the milestone made, and what's left for later. No MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| Can app code reach a database only through SQL in `code/`, with values always bound? | **Yes.** `db.query(name, values)` runs a `NamedQuery` the compiler made from `code/queries/`; there's no way to run a string. Values are parameters; name slots insert only names from the query's own list. |
| Does the same app run on SQLite and Postgres? | **Yes.** One placeholder syntax for both, variants (`.sqlite.sql`, `.pg.sql`) where SQL differs, and the same shared query tests pass on both. |
| Does a migration that fails partway leave the data exactly as it was? | **Yes**, on both databases: tested by failing each step in turn (the backup, before and after each of three migrations of each kind, the commit), comparing every data file and the whole database before and after. |
| Does a long migration under load hold requests, then serve them on the new version? | **Yes.** Four threads of requests through a 1.5-second migration: none failed, requests that arrived meanwhile waited and got the new version, and a wait over the limit gets a 503 (`pages/503.html`). |
| Is a version that can't run on the data refused? | **Yes:** an applied migration edited or gone, one numbered below an applied one, or code older than the data (overridable with `--allow-older-code`). |

---

## What was built

- **`compiledapp`:** `sql.h`: `SqlDialect`; `prepareSql`, a scanner that reads SQL as each database does (strings, `E'...'`, dollar quotes, quoted names, `[names]` and backquotes on SQLite, nested comments on Postgres), finds `{{key}}` placeholders and `{{key: a, b}}` name slots, rewrites placeholders to `?N` or `$N`, counts statements, and reports mistakes by line; `bindValues` and `bindQuery`. `NamedQuery`, `QuerySet`, `Migration`; on `CompiledApp`, `dialect`, `queries`, `migrations`, `seed`, `unavailablePage` (`pages/503.html`).
- **`compiler`:** `pass_database.cpp`: queries (names from paths, variants chosen for the app's database, keys compared across variants, one statement each), migrations (numbering, kinds, folders, checksums of what's applied), and `code/seed.ms`. `CompileOptions::dialect`.
- **`platform`:** the SQLite wrapper gained columns and types, doubles, one-off statements, a deadline (through the progress handler), and the online backup API (`backupTo`, `restoreFrom`).
- **`partition`:** `DataDir` has a host path, engine files in `data/.fourfold/`, the database's path, and whole-tree copies for backups (`copyOut`, `copyIn`, `clear`; `tree_copy.cpp`, one segment at a time, links never followed). `StateDir::folderPath`, `remove`, and the same copies.
- **`appdata`:** `Database` (`database.h`): SQLite (a pool over one file) and Postgres (libpq, when found at build time), `DbConnection` for transactions, limits on rows, bytes and time, and a lazily opened wrapper. `Migrator`: records in a `fourfold_migrations` table and in `data/.fourfold/migrations.json`, `planMigrations`, the batch transaction (with Postgres's advisory lock), a fault injector for tests. `BackupService`: backups, restores, listing, pruning.
- **`script`:** the `db` module (`db.query`). `ScriptSources`: migrations and the seed are compiled with the app; a migration may import only its folder's helpers, and a single file can't name `db` (both compile errors). `runMigration` and `runSeed`: a migration's top level, then `migrate()`, with its own queries, data files and its own budget; no session, users or rate limits. `RunContext` has `db`, `queries` and a `budget` override.
- **`config`:** `config.db` (`url`, `passwordSecret`, `schema`, `pool`, `queryTimeoutSeconds`, `maxRows`), `config.deploy` (`migrationWaitSeconds`, `migrationBudgetSeconds`, `clearAnonymousSessions`), `config.backups` (`folder`, `everyHours`, `keep`).
- **`pipeline`:** `SchemaGate`: every request past the reserved paths holds it shared; pausing it queues new requests, waiting for those inside; a timer thread turns away requests that wait too long, with the app's 503 page. Request runs get the database and the app's queries.
- **`lifecycle`:** `AppInstance` checks each compiled version against the applied migrations, then swaps it in, migrates first (dev), or holds it (production) until `migrateNow()`. Drain, backup, migrate, swap; on failure, data files restored and the old version resumed. `backupNow`, `redoLast`, `resetData`, `migrationStatus`; the seed on a fresh dev database. `DataServices`.
- **`state`:** `SessionStore::removeAnonymous`.
- **`devtools`:** `/__dev/migrations`: each migration's state (applied, pending, failed, changed, missing), the newest version's problems, and "redo the last migration" and "reset dev data" (POST, same origin only).
- **`hosts/cli`:** `fourfold migrate`, `backup` and `restore` (`data_commands.cpp`); `serve --migrate` and `--allow-older-code`. `AppServer` holds the app's server lock, opens the database lazily, sets up backups, answers control requests (`private/state/control/*.request`, checked every 250 ms), and takes scheduled backups.
- **Sample:** `examples/tasks`, a to-do list: migrations of all three kinds, variants, named queries, the seed, and a 503 page.

## Choices made here

- **Placeholders** (decided with the user before building; overview §3, SQL dialects): `{{key}}`, a name or an index, the only syntax; values in a map, or a list when every key is an index; a number key in a map counts as its string. `{{` in quotes is a compile error. The scanner also refuses each database's own parameter syntax (`?`, `:x`, `@x` on SQLite; `$1` on Postgres), which would otherwise be unbound.
- **Name slots** (decided with the user during M8): the allowlist inline, `{{sort: title, created}}`. The chosen name is inserted **as written**, not quoted, so it means what it would if typed there (quoting would change it on Postgres, which folds unquoted names to lowercase). Names are plain (`title`) or dotted (`notes.title`); `asc` and `desc` need no special case. A key can't be both a value and a slot.
- **Migrations bring their own SQL** (decided with the user): a folder with `migrate.ms`, its queries and helpers; imports from outside it are compile errors.
- **One transaction per batch.** The overview had one per migration. With Postgres, which isn't in Fourfold's backups, a batch failing at its second migration would have left the first applied while the old code resumed. So the batch's database work (statements, `db.query` calls, and the records) is one transaction; data files changed by `.ms` migrations come back from the backup. Postgres takes an advisory lock for the batch, so servers sharing a database migrate once.
- **Explicit migrations in production** (decided with the user): a version with pending migrations compiles but waits; `fourfold migrate` asks the running server through a request file (`private/state/control/migrate.request`, answered in `migrate-<id>.result`), or, with no server running, does the work itself holding the server lock. `serve` won't start with migrations pending unless `--migrate`.
- **Where SQLite lives:** `data/.fourfold/app.sqlite`, out of `file`'s and `data`'s reach (RelPath can't name dot-folders), opened on first use, so apps without a database get no file.
- **What `db.query` returns:** always a list of row maps. Booleans and json/jsonb columns on Postgres come back as 1/0 and parsed values; binary columns are an error. Whole numbers are bound as integers. Limits: 10,000 rows, 16 MB, and the request's time (SQLite's progress handler; Postgres's `statement_timeout`).
- **The schema gate** covers every request but `/__dev/` and `/__fourfold/`, including the sign-in pages. A request is pinned to the app current when it's admitted, so held requests go on to the new version.
- **Backups** are folders, not archives, written under a temporary name and renamed when whole. Automatic ones are pruned; manual and "before-restore" ones never. A restore needs the server stopped, and backs up what's there first. In-process restores (a failed migration) put back data files only.
- **Dev tools:** "redo last" restores the newest "before-migrations" backup and recompiles; "reset dev data" empties `data/` (after a backup). SQLite only.
- **The test Postgres:** the sandbox can't start a private server (no shared memory), so the Postgres tests use `FOURFOLD_TEST_PG_URL` (the user's local server, database `fourfold_test`, approved), each in a throwaway schema.

## Tests

- `tests/compiledapp/sql_test.cpp`: placeholders rewritten per database; strings, quoted names, dollar quotes and comments left alone; placeholders in quotes refused; the databases' own parameters refused; statements counted; binding from lists and maps; name slots chosen, and anything off the list refused; mistakes in slots.
- `tests/compiler/database_test.cpp`: variants chosen per database; keys compared across them; mistakes in queries; migrations of each kind, sorted by number; checksums following what's applied (another database's variant ignored); every kind of migration mistake, including importing app code and a single file using `db`.
- `tests/appdata/database_test.cpp`: **the shared query tests**, on SQLite and on Postgres: inserts with `RETURNING`, types both ways, injection attempts kept as text, JSON values, constraint errors, transactions committed, rolled back, and abandoned, eight threads at once; row and time limits.
- `tests/appdata/migrator_test.cpp`: plans (changed, missing, renumbered, older code and its override); the batch on both databases, with failures from a script and from SQL, and injected at every step; records in the database and in `data/.fourfold/`; backups taken, restored (data only, and with state), listed and pruned.
- `tests/hosts/cli/database_test.cpp`, end to end (**the exit criteria**):
  - dev mode: migrations and the seed on first start; named queries, name slots and error values; a new migration applied on recompile; an edited one refused; redo last; the panel, and its buttons refused from another origin; reset dev data;
  - a failure at each step leaves SQLite, Postgres and the data files exactly as they were;
  - a long migration under load (requested with `fourfold migrate`, through the running server) holds requests and serves them on the new version; one that runs past the wait limit gets the app's 503;
  - `serve` refuses pending migrations; `fourfold migrate` with no server; older code refused, and the override;
  - `fourfold backup` (offline and through the server) and `fourfold restore`, refused while the server runs;
  - the tasks sample, on a fresh copy.

All 21 suites pass with the `dev` and `asan` presets, with `FOURFOLD_TEST_PG_URL` set.

## Still to do

- Several queries from one request as one transaction; binary columns; leaving sessions out of a backup; copying a SQLite app's rows into Postgres (all in the overview's Still open).
- CGI mode (M10): requests get a 503 while migrations are pending, and `fourfold migrate` takes the schema lock as a file lock. The M5 CGI spike doesn't use a database yet.
- The key-value store of Tier 2.

## Still to check by hand

- **`examples/tasks` on Postgres**, from a browser: `config.db = {"url": "postgres://localhost/tasks"}` after `createdb tasks`.
- **The migrations panel**: edit `examples/tasks/code/migrations/002_add_rank.sql` while `fourfold dev` runs, see the refusal, then "redo the last migration".

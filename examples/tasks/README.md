# Tasks (M8 sample)

A to-do list kept in a database: SQLite by default, Postgres if
`private/config/app.ms` says so.

```sh
./build/dev/bin/fourfold dev examples/tasks
```

Then open http://localhost:8090/, and http://localhost:8090/__dev/migrations.

- The first start makes `data/.fourfold/app.sqlite` and applies the three
  migrations (with a backup first, in `private/state/backups/`), then runs
  `code/seed.ms`, which adds the sample tasks. The terminal says so.
- Every query is a file in `code/queries/`, run by name with `db.query`.
  Values go in through placeholders, `{{title}}` or `{{0}}`, and are always
  bound, never pasted into the SQL: try adding a task called
  `'); DROP TABLE tasks; --`.
- `addTask` has two variants, `addTask.sqlite.sql` and `addTask.pg.sql`,
  since the two databases write "now" differently; they take the same
  values. So does `001_create_tasks`.
- `003_rank_urgent/` is a folder migration: `migrate.ms` with its own
  queries and helper, so it can't be broken by later changes to the app's.
- Add a migration while `fourfold dev` runs (say
  `004_notes.sql`: `ALTER TABLE tasks ADD COLUMN notes TEXT;`) and it's
  applied as soon as you save. Edit it afterward, and the compile refuses:
  "redo the last migration" on `/__dev/migrations` puts the data back as it
  was before it, and applies your new version. "Reset dev data" starts over
  from the seed.
- In production, migrations wait for you: `fourfold serve` won't start with
  any pending, and a deploy that brings new ones keeps serving the old
  version until `fourfold migrate examples/tasks`.

To try Postgres, create a database and add to `private/config/app.ms`:

```
config = {}
config.db = {"url": "postgres://localhost/tasks"}
```

| File | Shows |
|---|---|
| `migrations/001_create_tasks.sqlite.sql`, `.pg.sql` | a migration with a variant for each database (and a `STRICT` table on SQLite) |
| `migrations/002_add_rank.sql` | one migration for both |
| `migrations/003_rank_urgent/` | a folder migration: `migrate.ms`, its queries, and a helper it imports |
| `queries/*.sql` | named queries; `{{name}}` placeholders with a map, `{{0}}` with a list |
| `seed.ms` | sample data for a fresh dev database |
| `pages/index.ms` | `db.query`, and error values from it; declared forms; HTMX redrawing the board |
| `pages/503.html` | what visitors see if migrations take longer than they'll wait |

-- The tasks table, on SQLite. (001_create_tasks.pg.sql is the same table
-- for Postgres, whose auto-numbered key is written differently.)
CREATE TABLE tasks (
    id      INTEGER PRIMARY KEY,
    title   TEXT NOT NULL,
    done    INTEGER NOT NULL DEFAULT 0,
    created TEXT NOT NULL
) STRICT;

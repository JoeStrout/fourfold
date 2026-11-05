-- The tasks table, on Postgres.
CREATE TABLE tasks (
    id      SERIAL PRIMARY KEY,
    title   TEXT NOT NULL,
    done    INTEGER NOT NULL DEFAULT 0,
    created TEXT NOT NULL
);

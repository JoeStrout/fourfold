-- The time is the database's own; SQLite and Postgres write it differently,
-- hence the two variants. Both take the same values.
INSERT INTO tasks (title, rank, created) VALUES ({{title}}, {{rank}}, datetime('now'))
RETURNING id, title, done, rank

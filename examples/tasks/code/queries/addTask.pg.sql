INSERT INTO tasks (title, rank, created) VALUES ({{title}}, {{rank}}, to_char(now() AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'))
RETURNING id, title, done, rank

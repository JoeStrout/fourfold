-- Open tasks first, then by rank, then oldest first.
SELECT id, title, done, rank FROM tasks ORDER BY done, rank, id

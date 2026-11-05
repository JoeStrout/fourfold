UPDATE tasks SET done = 1 - done WHERE id = {{0}} RETURNING id, title, done, rank

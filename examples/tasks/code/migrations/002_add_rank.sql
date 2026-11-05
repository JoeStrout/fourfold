-- Tasks get a rank: 1 is urgent, 2 normal. The same SQL works on both
-- databases, so one file does.
ALTER TABLE tasks ADD COLUMN rank INTEGER NOT NULL DEFAULT 2;

// Sample tasks, for a fresh database in dev mode (after "reset dev data" on
// /__dev/migrations, too). Never runs in production.
for t in [["Water the plants", 2], ["Pay the electric bill TODAY", 1], ["Read a book", 2]]
    db.query "addTask", {"title": t[0], "rank": t[1]}
end for

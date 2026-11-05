// A backfill that needs logic: tasks written before ranks existed, whose
// titles say they're urgent, get rank 1. It uses only its own queries
// (all.sql, setRank.sql) and its own helper (words.ms), so it runs the same
// way years from now, whatever happens to code/queries/ and code/lib/.
import "words"

migrate = function
    for task in db.query("all")
        if words.soundsUrgent(task.title) then
            db.query "setRank", {"id": task.id, "rank": 1}
        end if
    end for
end function

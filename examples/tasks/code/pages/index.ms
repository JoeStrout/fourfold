// A to-do list in a database (M8): every read and write is a named query
// from code/queries/, with its values bound, never pasted into the SQL.

form.add = {
    "title":  {"type": "text", "required": true, "max": 200,
               "messages": {"required": "What's the task?"}},
    "urgent": {"type": "bool"},
}
form.toggle = {"id": {"type": "int", "required": true}}
form.remove = {"id": {"type": "int", "required": true}}

// One task as frags/task.html wants it.
show = function(task)
    cls = ""
    if task.done then cls = "done"
    if task.rank == 1 and not task.done then cls = "urgent"
    mark = "○"
    if task.done then mark = "●"
    return {"id": task.id, "title": task.title, "class": cls, "mark": mark}
end function

// The board's fields: the add form, the list, and the count. A failed
// query is an error value, which becomes the error page.
board = function
    tasks = db.query("listTasks")
    if tasks isa error then return tasks
    shown = []
    for t in tasks
        shown.push show(t)
    end for
    return {
        "addForm": frag("addForm", formState("add")),
        "tasks": frag("task", shown),
        "open": db.query("countOpen")[0].n,
    }
end function

render = function(req)
    fields = board
    if fields isa error then return fields
    return {"board": frag("board", fields)}
end function

// Each post answers HTMX with the board, redrawn; without JavaScript, it
// sends the browser back to the page.
answer = function(req)
    if not req.htmx then return redirect("/")
    fields = board
    if fields isa error then return fields
    return frag("board", fields)
end function

post.add = function(req)
    if req.errors then return reshow("addForm")
    rank = 2
    if req.form.urgent then rank = 1
    added = db.query("addTask", {"title": req.form.title, "rank": rank})
    if added isa error then return added
    return answer(req)
end function

post.toggle = function(req)
    if not req.errors then db.query "toggleTask", [req.form.id]
    return answer(req)
end function

post.remove = function(req)
    if not req.errors then db.query "deleteTask", [req.form.id]
    return answer(req)
end function

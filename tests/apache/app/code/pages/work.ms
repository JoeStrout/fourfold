get.queue = function(req)
    enqueue "note", {"text": req.query.t}
    return "queued"
end function

get.notes = function(req)
    return data.get("note", "none")
end function

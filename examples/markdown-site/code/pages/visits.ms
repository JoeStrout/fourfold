// The visitor counter: visits.md's fields, filled. The page is served at
// /visits as HTML and at /visits.md as Markdown, by the same code.
render = function(req)
    return {"visits": data.increment("visits"), "format": req.format.upper}
end function

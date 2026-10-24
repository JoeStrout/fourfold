// GET /api/echo: what the handler sees of the request, as JSON.
get.echo = function(req)
    userAgent = ""
    if req.headers.hasIndex("user-agent") then userAgent = req.headers["user-agent"]
    return json({"method": req.method, "path": req.path, "query": req.query, "userAgent": userAgent})
end function

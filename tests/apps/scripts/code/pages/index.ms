import "util"

render = function(req)
    name = "world"
    if req.query.hasIndex("name") then name = req.query.name
    return {"greeting": util.greet(name), "count": 3}
end function

// The navigation bar: who's signed in, with a way out; or a way in.
nav = function(req)
    if req.user == null then return frag("nav/out", {"next": req.path})
    return frag("nav/in", {"name": req.user.name, "roles": req.user.roles.join(", ")})
end function

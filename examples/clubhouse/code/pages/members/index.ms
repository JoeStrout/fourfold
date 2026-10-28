import "nav"

// Only signed-in visitors get here (code/access.ms), so req.user is set.
render = function(req)
    return {"nav": nav.nav(req), "name": req.user.name, "email": req.user.email}
end function

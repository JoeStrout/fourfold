import "nav"

render = function(req)
    greeting = "Welcome, visitor. Sign in to see more."
    if req.user != null then greeting = "Welcome back, " + req.user.name + "."
    return {"nav": nav.nav(req), "greeting": greeting}
end function

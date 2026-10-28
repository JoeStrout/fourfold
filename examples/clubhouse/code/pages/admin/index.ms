// The admin area. code/access.ms already says "admin" for /admin; saying it
// here too is harmless (rules only tighten), and keeps the page safe if the
// central rule is ever changed. Admins are named in private/config/app.ms.
access = "admin"

import "nav"

render = function(req)
    return {"nav": nav.nav(req)}
end function

change = function(req, grant)
    email = ""
    if req.form.hasIndex("email") then email = req.form.email
    u = users.find(email)
    if u == null then
        session.flash = "There's no user with that email address (they need to sign in once first)."
    else if grant then
        users.grant u.id, "moderator"
        session.flash = u.name + " is a moderator now."
    else
        users.revoke u.id, "moderator"
        session.flash = u.name + " isn't a moderator any more."
    end if
    return redirect("/admin/")
end function

post.grant = function(req)
    return change(req, true)
end function

post.revoke = function(req)
    return change(req, false)
end function

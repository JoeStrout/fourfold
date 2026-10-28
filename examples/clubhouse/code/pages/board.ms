// A public board. Reading is open to all; post.post is for signed-in
// visitors only, by the rule for /board/post in code/access.ms, so it never
// runs for anyone else. The poster's name comes from req.user, not the form.
import "nav"

render = function(req)
    form = frag("board/signin")
    if req.user != null then form = frag("board/form", {"name": req.user.name})
    return {"nav": nav.nav(req), "posts": frag("board/post", data.get("board", [])), "form": form}
end function

post.post = function(req)
    text = ""
    if req.form.hasIndex("text") then text = req.form.text
    if text.len == 0 or text.len > 500 then return status(422, "Say something (up to 500 characters).")
    data.append "board", {"name": req.user.name, "user": req.user.id, "text": text}
    session.flash = "Posted."
    return redirect("/board")
end function

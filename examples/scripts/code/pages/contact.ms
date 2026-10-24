// POST /contact/send
post.send = function(req)
    if not req.form.hasIndex("name") or req.form.name == "" then
        return status(400, "Please give your name.")
    end if
    message = ""
    if req.form.hasIndex("message") then message = req.form.message
    print "Message from " + req.form.name + ": " + message
    // Redirect after a POST, so reloading the next page doesn't send it again.
    return redirect("/contact/thanks")
end function

// GET /contact/thanks
get.thanks = function(req)
    return "Thanks! Your message was received."
end function

form.send = {
    "from":    {"type": "email", "required": true},
    "message": {"type": "multiline", "required": true, "max": 2000, "maxLinks": 2},
}

post.send = function(req)
    if req.errors then
        session.flash = "Please give your address and a message."
        return redirect("/contact")
    end if
    // Visitors' text goes in as fields, never as headers: nothing typed
    // here can add a recipient.
    sent = mail.send("owner@example.com", "A message from the weather site", "contact", req.form)
    if sent isa error then
        session.flash = "Sorry, that couldn't be sent: " + sent.message
    else
        session.flash = "Thanks! Your message was sent."
    end if
    return redirect("/contact")
end function

// The guestbook from the overview (§7). Entries are kept in data/, as a log
// (data/guestbook.jsonl): data.append adds one, data.get reads them all.
//
// The form is declared (M7): the engine trims and checks each field before
// post.sign runs, drops anything undeclared, and puts what's wrong in
// req.errors. code/limits.ms adds spam rules for /sign.

nl = char(10)

form.sign = {
    "who":   {"type": "text", "required": true, "max": 40,
              "messages": {"required": "Please sign your name."}},
    "msg":   {"type": "multiline", "required": true, "max": 2000, "maxLinks": 2},
    "mood":  {"type": "choice", "options": ["happy", "meh", "grumpy"]},
    "shout": {"type": "bool"},
}

// The first visit seeds the guestbook with these. Some try to break the
// page; the templates and Markdown keep them as text.
samples = [
    {"who": "Ada", "msg": "What a **lovely** guestbook. See [the docs](https://example.com)."},
    {"who": "<script>alert('hi')</script>", "msg": "My name is a script tag. It shows as text."},
    {"who": "Mallory", "msg": "[Click me](javascript:alert(1)) and ![tracker](https://evil.example/pixel.gif)"},
    {"who": "Grace", "msg": "- lists" + nl + "- `code`" + nl + "- ~~mistakes~~" + nl + nl + "# Not a heading, in a comment"},
]

// The entries, seeding them on the first visit. compareAndSet succeeds for
// exactly one request, so two first visits at once can't both seed.
entries = function
    if data.compareAndSet("seeded", null, 1) then
        for e in samples
            data.append "guestbook", e
        end for
    end if
    return data.get("guestbook", [])
end function

// One entry as the entry fragment wants it: the message rendered from
// Markdown, so it goes in as HTML, not as text.
showEntry = function(e)
    mood = ""
    if e.hasIndex("mood") and e.mood != null then mood = e.mood
    return {"who": e.who, "msg": markdown(e.msg), "mood": mood}
end function

render = function(req)
    list = entries
    shown = []
    for e in list
        shown.push showEntry(e)
    end for
    signed = 0
    if session.hasIndex("signed") then signed = session.signed
    return {
        "entries": frag("entry", shown),
        "count": list.len,
        "visits": data.increment("visits"),
        "signed": signed,
        // The form, with what the visitor typed and what was wrong, right
        // after a failed submission without JavaScript; else empty.
        "signForm": frag("signForm", formState("sign")),
        "stats": {"entries": list.len, "names": list.indexes.len},
    }
end function

post.sign = function(req)
    // The declared rules failed: the form again, with the errors and the
    // visitor's input (in place with HTMX; on the page without it).
    if req.errors then return reshow("signForm")
    // A check only the handler can make.
    if req.form.who.lower == "admin" then return reshow("signForm", {"who": "That name is reserved."})
    msg = req.form.msg
    if req.form.shout then msg = msg.upper
    entry = {"who": req.form.who, "msg": msg, "mood": req.form.mood}
    data.append "guestbook", entry
    // This visitor's own count, in their session.
    if not session.hasIndex("signed") then session.signed = 0
    session.signed += 1
    // With HTMX, just the new <li>; without it, back to the page, with a
    // message to show there once.
    if req.htmx then return frag("entry", showEntry(entry))
    session.flash = "Thanks for signing, " + req.form.who + "!"
    return redirect("/")
end function

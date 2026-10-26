// The guestbook from the overview (§7). Entries are kept in data/, as a log
// (data/guestbook.jsonl): data.append adds one, data.get reads them all.

nl = char(10)

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
    return {"who": e.who, "msg": markdown(e.msg)}
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
        "shoutByDefault": false,
        "stats": {"entries": list.len, "names": list.indexes.len},
    }
end function

post.sign = function(req)
    who = ""
    if req.form.hasIndex("who") then who = req.form.who
    msg = ""
    if req.form.hasIndex("msg") then msg = req.form.msg
    if req.form.hasIndex("shout") then msg = msg.upper
    if who == "" or msg == "" then return status(422, "Please give a name and a message.")
    entry = {"who": who, "msg": msg}
    data.append "guestbook", entry
    // This visitor's own count, in their session.
    if not session.hasIndex("signed") then session.signed = 0
    session.signed += 1
    // With HTMX, just the new <li>; without it, back to the page, with a
    // message to show there once.
    if req.htmx then return frag("entry", showEntry(entry))
    flash "Thanks for signing, " + who + "!"
    return redirect("/")
end function

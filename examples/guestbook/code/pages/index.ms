// The guestbook from the overview (§7), with fixed sample entries: storing
// new ones needs `data`, which arrives in M5.

nl = char(10)

// Some entries try to break the page; the templates and Markdown keep them
// as text.
samples = [
    {"who": "Ada", "msg": "What a **lovely** guestbook. See [the docs](https://example.com)."},
    {"who": "<script>alert('hi')</script>", "msg": "My name is a script tag. It shows as text."},
    {"who": "Mallory", "msg": "[Click me](javascript:alert(1)) and ![tracker](https://evil.example/pixel.gif)"},
    {"who": "Grace", "msg": "- lists" + nl + "- `code`" + nl + "- ~~mistakes~~" + nl + nl + "# Not a heading, in a comment"},
]

// One entry as the entry fragment wants it: the message rendered from
// Markdown, so it goes in as HTML, not as text.
showEntry = function(e)
    return {"who": e.who, "msg": markdown(e.msg)}
end function

render = function(req)
    entries = []
    for e in samples
        entries.push showEntry(e)
    end for
    return {
        "entries": frag("entry", entries),
        "count": samples.len,
        "shoutByDefault": false,
        "stats": {"entries": samples.len, "names": samples.indexes.len},
    }
end function

post.sign = function(req)
    who = ""
    if req.form.hasIndex("who") then who = req.form.who
    msg = ""
    if req.form.hasIndex("msg") then msg = req.form.msg
    if req.form.hasIndex("shout") then msg = msg.upper
    if who == "" or msg == "" then return status(422, "Please give a name and a message.")
    // With HTMX, just the new <li>; without it, back to the page.
    if req.htmx then return frag("entry", showEntry({"who": who, "msg": msg}))
    return redirect("/")
end function

// A guestbook listing, entirely in Markdown: the page (guestbook.md), and a
// fragment per entry (frags/entry.md). Entries are kept in data/, as a log.
// (A Markdown page can't have a form yet, so this one only reads.)

nl = char(10)

// The first visit seeds the guestbook with these. Some try to break the
// page; the templates and Markdown keep them as text.
samples = [
    {"who": "Ada", "msg": "What a **lovely** guestbook. See [the docs](https://example.com)."},
    {"who": "<script>alert('hi')</script>", "msg": "My name is a script tag. It shows as text."},
    {"who": "Mallory", "msg": "[Click me](javascript:alert(1)) and ![tracker](https://evil.example/pixel.gif)"},
    {"who": "**Grace**", "msg": "- lists" + nl + "- `code`" + nl + nl + "# Not a heading, in a message"},
]

entries = function
    if data.compareAndSet("seeded", null, 1) then
        for e in samples
            data.append "guestbook", e
        end for
    end if
    return data.get("guestbook", [])
end function

render = function(req)
    shown = []
    for e in entries
        shown.push {"who": e.who, "msg": markdown(e.msg)}
    end for
    return {"count": shown.len, "entries": frag("entry", shown)}
end function

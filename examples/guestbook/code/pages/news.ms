// The news page is Markdown kept in data/news.md, a file the app owns
// (M5.1): change that file and reload, with no redeploy. It's data, so
// nothing in it runs, and no URL serves it directly; this page reads it with
// `file` and renders it. The first visit writes a starter version.

starter = [
    "# News",
    "",
    "This page is **data/news.md**, read by `news.ms` with `file.readLines`",
    "and shown with `markdown(lines, ""full"")`.",
    "",
    "Edit the file and reload to see the change.",
]

render = function(req)
    lines = file.readLines("news.md")
    if lines == null then
        file.writeLines "news.md", starter
        lines = starter
    end if
    return {"news": markdown(lines, "full")}
end function

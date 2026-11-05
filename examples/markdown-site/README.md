# Markdown site (M4 and M6.1 sample)

A site made only of `.md` files: no HTML and no layout, and code only where a
page needs it. See [FourFold Flavored Markdown](../../docs/user/MARKDOWN.md).

```sh
./build/dev/bin/fourfold dev examples/markdown-site
```

- Each `pages/*.md` is served at its name (`guide/start.md` at `/guide/start`),
  in a minimal built-in page with the default Markdown stylesheet.
- Links between `.md` files are rewritten to URLs, and a link to a page that
  doesn't exist is a warning (`start.md` has one on purpose).
- Add `code/frags/layout/markdown.html` (with `{{content}}` and `{{title}}`) to
  use your own layout, or `code/pages/markdown.css` for your own styles.
- Every page is also served as Markdown, at its name with `.md`:
  `/guide/start.md`, and `/index.md` for the home page. Try
  `curl localhost:8090/visits.md`.
- `visits.md` is a visitor counter: a Markdown page with fields, filled by
  `render` in `visits.ms`.
- `guestbook.md` lists a guestbook's entries, each rendered from
  `frags/entry.md`, with visitors' messages as Limited Markdown. (Markdown has
  no forms, so it only reads.)

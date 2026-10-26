# Markdown site (M4 sample)

A site made only of `.md` files: no HTML, no code, no layout.

```sh
./build/dev/bin/fourfold dev examples/markdown-site
```

- Each `pages/*.md` is served at its name (`guide/start.md` at `/guide/start`),
  in a minimal built-in page with the default Markdown stylesheet.
- Links between `.md` files are rewritten to URLs, and a link to a page that
  doesn't exist is a warning (`start.md` has one on purpose).
- Add `code/frags/layout/markdown.html` (with `{{content}}` and `{{title}}`) to
  use your own layout, or `code/pages/markdown.css` for your own styles.

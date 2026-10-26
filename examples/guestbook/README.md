# Guestbook (M4 sample)

The guestbook from the design notes (overview §7): templates with fields,
fragments, an include, Markdown, and HTMX.

```sh
./build/dev/bin/fourfold dev examples/guestbook
```

Then open http://localhost:8090/.

- The sample entries include some that try to break the page (a `<script>` for
  a name, a `javascript:` link, a tracking image). They show as text.
- Sign the guestbook: with JavaScript, HTMX posts the form and appends the new
  entry in place. With JavaScript off, the form posts normally and the handler
  redirects back. It's the same handler either way (`req.htmx` tells them
  apart). New entries aren't stored yet; that needs `data`, in M5.
- `/about` is a Markdown page, wrapped in `frags/layout/markdown.html`.

| File | Shows |
|---|---|
| `pages/index.html` | fields in text, a boolean attribute (`checked="{{x}}"`), JSON in a `<script>`, `{{> header}}`, HTMX loaded from `/__fourfold/htmx.js` |
| `pages/index.ms` | `frag("entry", list)`, `markdown(...)`, `req.htmx`, `status(422, ...)`, `redirect(...)` |
| `frags/entry.html` | the one place an entry's HTML lives, used for the page and for HTMX |
| `frags/header.html` | a static include |
| `pages/about.md`, `frags/layout/markdown.html` | a Markdown page in the app's own layout |
| `pages/guestbook.js` | reading the page's JSON from a script file (inline scripts need the engine's nonce) |

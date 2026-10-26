# Guestbook (M4 and M5 sample)

The guestbook from the design notes (overview §7): templates with fields,
fragments, an include, Markdown, and HTMX; and (M5) entries stored in
`data/`, a visit counter, the visitor's own count in `session`, and a flash
message.

```sh
./build/dev/bin/fourfold dev examples/guestbook
```

Then open http://localhost:8090/.

- The sample entries include some that try to break the page (a `<script>` for
  a name, a `javascript:` link, a tracking image). They show as text.
- Sign the guestbook: with JavaScript, HTMX posts the form and appends the new
  entry in place. With JavaScript off, the form posts normally and the handler
  redirects back, with a thank-you shown once (`session.flash`). It's the same handler
  either way (`req.htmx` tells them apart).
- Entries are appended to `data/guestbook.jsonl`, and survive restarts. The
  first visit seeds the sample entries; `data.compareAndSet` makes sure only
  one request does. Every page view counts in `data/visits.json`
  (`data.increment`).
- Your own number of signatures is kept in your session (`session.signed`).
  The session starts with your first signature; until then, you have none.
- Forms carry a CSRF token the engine adds when it renders the page, and
  HTMX sends it in a header (the engine sets `hx-headers` on `<body>`). A
  form posted from another site is refused before the handler runs.
- `/about` is a Markdown page, wrapped in `frags/layout/markdown.html`.

| File | Shows |
|---|---|
| `pages/index.html` | fields in text, a boolean attribute (`checked="{{x}}"`), JSON in a `<script>`, `{{> header}}`, HTMX loaded from `/__fourfold/htmx.js` |
| `pages/index.ms` | `frag("entry", list)`, `markdown(...)`, `req.htmx`, `status(422, ...)`, `redirect(...)`; `data.get`, `data.append`, `data.increment`, `data.compareAndSet`; `session`, including `session.flash` |
| `frags/entry.html` | the one place an entry's HTML lives, used for the page and for HTMX |
| `frags/header.html` | a static include |
| `pages/about.md`, `frags/layout/markdown.html` | a Markdown page in the app's own layout |
| `pages/guestbook.js` | reading the page's JSON from a script file (inline scripts need the engine's nonce) |

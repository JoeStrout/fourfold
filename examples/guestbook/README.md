# Guestbook (M4, M5, M5.1 and M7 sample)

The guestbook from the design notes (overview §7): templates with fields,
fragments, an include, Markdown, and HTMX; (M5) entries stored in `data/`, a
visit counter, the visitor's own count in `session`, and a flash message; and
(M5.1) a news page kept as a Markdown file in `data/`.

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
- (M7) The form is declared (`form.sign` in `index.ms`): the engine trims and
  checks each field before `post.sign` runs, drops anything undeclared, and
  adds `required` and `maxlength` to the inputs. Leave the name empty, or
  paste three links into the message, and the form comes back with the
  errors and what you typed: in place with JavaScript, on the reloaded page
  without (`reshow`, `formState`). The name "admin" is refused by the
  handler itself.
- (M7) Spam rules in `code/limits.ms`: signing waits at least 2 seconds
  after the form was shown, and is limited to 5 a minute and 50 a day per
  visitor; over the limit, `pages/429.html`.
- (M7) `/photos` takes a PNG or JPEG upload, checked by its content, saved in
  `data/photos/`, and shown with `file.send`. Its route asks for a
  proof-of-work puzzle, which the page's JavaScript solves while you pick a
  file.
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
- `/news` is Markdown too, but kept in `data/news.md`, which the app owns:
  `news.ms` reads it with `file.readLines` and renders it with `markdown()`.
  Edit the file and reload; no redeploy. The first visit writes a starter
  version. The file itself is never served by URL.

| File | Shows |
|---|---|
| `pages/index.html` | fields in text, a boolean attribute (`checked="{{x}}"`), JSON in a `<script>`, `{{> header}}`, HTMX loaded from `/__fourfold/htmx.js` |
| `pages/index.ms` | `form.sign`, `req.errors`, `reshow`, `formState`; `frag("entry", list)`, `markdown(...)`, `req.htmx`, `redirect(...)`; `data.get`, `data.append`, `data.increment`, `data.compareAndSet`; `session`, including `session.flash` |
| `frags/signForm.html` | the form as a fragment, with `{{errors.who}}` and the like, and `checked="{{mood.happy}}"` |
| `code/limits.ms`, `pages/429.html` | rate limits, the timing check, a puzzle, and the page for too many requests |
| `pages/photos.ms`, `frags/uploadForm.html` | a file upload (`req.files`), `file.saveRaw`, `file.send` |
| `frags/entry.html` | the one place an entry's HTML lives, used for the page and for HTMX |
| `frags/header.html` | a static include |
| `pages/about.md`, `frags/layout/markdown.html` | a Markdown page in the app's own layout |
| `pages/news.ms` | `file.readLines`, `file.writeLines`, and `markdown()` of a list of lines |
| `pages/guestbook.js` | reading the page's JSON from a script file (inline scripts need the engine's nonce) |

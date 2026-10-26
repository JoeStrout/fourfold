# Fourfold User Guide

How to build a web site with Fourfold: pages, templates, MiniScript code,
stored data and sessions. This guide is for the person writing the site. It
assumes Fourfold is already installed and serving your app with
`fourfold serve`; setting that up is covered in the admin guide.

*Status: covers what works as of Milestone 5. Features still to come (sign-in,
access rules, form validation, uploads, databases, background jobs and more)
will be added here as they arrive.*

MiniScript itself isn't covered here; see [miniscript.org](https://miniscript.org)
for the language. This guide covers what Fourfold adds to it.

---

## Contents

1. [The app folder](#the-app-folder)
2. [Pages and URLs](#pages-and-urls)
3. [Templates](#templates)
4. [Fragments and includes](#fragments-and-includes)
5. [Markdown](#markdown)
6. [Code: render and handlers](#code-render-and-handlers)
7. [The request: req](#the-request-req)
8. [What a handler returns](#what-a-handler-returns)
9. [Shared code: code/lib](#shared-code-codelib)
10. [Persistent site-wide data: data](#persistent-site-wide-data-data)
11. [Files in data/: file and RawData](#files-in-data-file-and-rawdata)
12. [Remembering visitors: session](#remembering-visitors-session)
13. [One-time messages: session.flash](#one-time-messages-sessionflash)
14. [Forms and CSRF protection](#forms-and-csrf-protection)
15. [HTMX: updating part of a page](#htmx-updating-part-of-a-page)
16. [JavaScript and the Content Security Policy](#javascript-and-the-content-security-policy)
17. [Output: print and the dropbox](#output-print-and-the-dropbox)
18. [Errors and limits](#errors-and-limits)
19. [Deploying changes](#deploying-changes)
20. [Developing on your own machine](#developing-on-your-own-machine)
21. [Quick reference](#quick-reference)

---

## The app folder

An app is one folder with four parts:

| Folder | Holds | Your code can |
|---|---|---|
| `code/` | Your site: pages, templates, MiniScript, images, CSS | nothing at run time; it's compiled when deployed |
| `data/` | What the app stores, through [`data`](#persistent-site-wide-data-data) and [`file`](#files-in-data-file-and-rawdata) | read and write it |
| `dropbox/` | The app's output: logs it writes, and `print` output | only add to it |
| `private/` | Fourfold's own files: sessions, logs, keys | nothing |

Inside `code/`:

| Folder | Holds |
|---|---|
| `code/pages/` | Everything with a URL: pages, their code, and static files |
| `code/frags/` | Pieces of HTML (and Markdown) that pages and code put together |
| `code/lib/` | MiniScript modules shared by pages |

Nothing outside `code/pages/` has a URL, and the only way anything in `data/`
reaches a visitor is through your code.

---

## Pages and URLs

### File-based routing

Each file in `code/pages/` is served at its path, with page extensions left
off:

| File | URL |
|---|---|
| `pages/index.html` | `/` |
| `pages/about.html` | `/about` |
| `pages/about.md` | `/about` (rendered Markdown; see [Markdown](#markdown)) |
| `pages/docs/index.html` | `/docs/` |
| `pages/docs/intro.html` | `/docs/intro` |
| `pages/img/logo.png` | `/img/logo.png` (static files keep their names) |

- **A folder's index page ends in `/`.** A request for `/docs` redirects to
  `/docs/`, and `/about/` redirects to `/about`. Each page has one URL, so
  relative links (`href="logo.png"`) always resolve against the page's own
  folder.
- **Old `.html` links still work.** `/about.html` redirects to `/about`, and
  `/docs/index.html` to `/docs/`. But write links without the extension, to
  save the redirect; the compile warnings point out links that use it.
- **Two files for one URL** (say `about.html` and `about.md`) is a compile
  error.
- **Reserved:** URLs starting with `/__fourfold/`, `/__dev/` or `/__auth/`
  belong to Fourfold. A folder of yours with one of those names is an error.
- **Broken links are compile warnings.** Every local link in your pages
  (`href`, `src`, `action`, `hx-get`, ...) is checked against what the app
  serves, and each one that leads nowhere is reported with its file and line.

### Static files

Images, CSS, JavaScript, fonts and other files in `pages/` are served as they
are. Keep them next to the pages that use them.

Only these types are served:

- **Pages and code:** `.html`, `.css`, `.js`, `.mjs`, `.json`, `.map`,
  `.webmanifest`, `.txt`, `.csv`, `.wasm`
- **Images:** `.png`, `.jpg`, `.jpeg`, `.gif`, `.webp`, `.avif`, `.svg`, `.ico`
- **Fonts:** `.woff`, `.woff2`, `.ttf`, `.otf`
- **Media:** `.mp3`, `.ogg`, `.wav`, `.mp4`, `.webm`
- **Downloads:** `.pdf`, `.zip`

Anything else gets a compile warning and isn't served. To offer such a file
for download, zip it.

`.ms` files are a page's code, and are never served.

### Error pages

- `pages/404.html`, if you have one, is shown for any URL the app doesn't
  serve.
- `pages/500.html`, if you have one, is shown when something goes wrong in
  your code. Without it, visitors see a short plain-text message. Either way,
  the details go to the engine log, never to the visitor (see
  [Errors and limits](#errors-and-limits)).

Neither page can have code yet. Fields in them render empty.

---

## Templates

HTML files in `pages/` and `frags/` are templates. A template is ordinary
HTML, plus **fields** where values go:

```html
<p>Hello, {{name}}! You are visitor number {{visits}}.</p>
```

Templates have **no logic**: no `if`, no loops, no expressions. Your code
decides what goes in each field (see [Code](#code-render-and-handlers)), and
fragments handle repetition (see [Fragments](#fragments-and-includes)).

### Fields

- `{{name}}` marks where a value goes. Spaces inside are fine: `{{ name }}`.
- `{{user.name}}` looks up `name` in the map `user`. That's the only
  operation.
- To write a literal `{{`, write `\{{`.
- A page with fields but no code is fine: its fields are simply empty.

### Every value is escaped for where it is

Fourfold reads each template when the app is compiled, works out what each
field is inside of (text, an attribute, a URL, a script), and escapes the
value to suit. Nothing a visitor types can become markup or script.

| Where | Example | What happens to the value |
|---|---|---|
| Text | `<p>{{msg}}</p>` | `& < > " '` are escaped; rendered HTML (from `frag`, `markdown`) goes in as HTML |
| `<title>`, `<textarea>` | `<title>{{t}}</title>` | escaped; rendered HTML is an error |
| A quoted attribute | `title="{{tip}}"`, `class="a {{b}}"` | escaped |
| A URL attribute, at the start | `href="{{link}}"` | must be `http:`, `https:`, `mailto:`, or a relative URL; anything else (`javascript:`, ...) becomes a harmless dead link |
| A URL attribute, later on | `href="/user/{{id}}?page={{n}}"` | percent-encoded, so `/ ? & =` in the value can't change the URL's shape |
| Inside `<script>` | `const cfg = {{config}};` | written as a JSON value; maps and lists work |

URL attributes are `href`, `src`, `action`, `formaction` and the like, any
attribute whose name contains `src`, `uri` or `url`, and HTMX's `hx-get`,
`hx-post` and friends.

**How values are written:**

- A **string** is escaped.
- A **number** is written the way MiniScript prints it.
- **`null`**, or a field your code didn't fill, is empty. If a field is an
  attribute's whole value and the value is null, the whole attribute is left
  out, so `class="{{highlight}}"` disappears when there's no highlight.
- **Boolean attributes** (`checked`, `disabled`, `selected`, `hidden`, ...)
  are switches: `<input type="checkbox" checked="{{agreed}}">` writes
  `checked` if the value is true (a non-zero number, or a non-empty string),
  and leaves it out otherwise.
- A **map or list** is an error, except inside `<script>`, where it becomes
  JSON.

### Where fields can't go

Anywhere a value couldn't be escaped safely is a **compile error**, with a
message saying what to do instead, so a dangerous template is never deployed:

- unquoted attribute values (`class={{c}}`), tag names, attribute names, and
  right after `<`;
- inside comments, `<style>`, `style="..."`, `srcset`, `srcdoc`, `<base>`, and
  `<meta http-equiv>`;
- inside `<svg>` or `<math>`;
- in HTMX attributes that can run JavaScript: `hx-vals`, `hx-headers`,
  `hx-trigger`, `hx-vars`;
- inside a JavaScript string, template literal, regular expression or comment
  in a `<script>`. Put the field where a whole value goes, then use it:
  `const name = {{name}};` rather than `"Hello, {{name}}"`.

Some HTML is refused outright, field or no field, because the
[Content Security Policy](#javascript-and-the-content-security-policy) would
block it anyway: `onclick=` and every other `on...` attribute, `hx-on...`
attributes, `javascript:` URLs, and a `nonce` attribute of your own.

### Pages must be whole documents

- A file in `pages/` must be a complete HTML document: it starts with
  `<!DOCTYPE html>` (or `<html>`).
- A file in `frags/` must **not** be: no doctype, `<html>`, `<head>` or
  `<body>`. And it must close everything it opens.

A piece of a page in `pages/` is a compile error that says to move it to
`frags/`.

### Missing and extra fields

When your code fills a template, a field it didn't give a value renders
empty, and a value with no matching field is ignored. Both are usually typos,
so both are reported as warnings while you're
[developing](#developing-on-your-own-machine).

---

## Fragments and includes

**Fragments** are pieces of HTML in `code/frags/`: a header, a list item, a
table row. They have no URL of their own. There are two ways to use one.

### Includes: `{{> name}}`

```html
<body>
  {{> header}}
  <main>...</main>
  {{> footer}}
</body>
```

`{{> header}}` pastes in `frags/header.html` when the app is compiled. It costs
nothing per request and needs no code, so even a site with no code at all can
share its header and footer. For a fragment in a subfolder, use its path:
`{{> layout/sidebar}}`.

- Fields inside an included fragment are filled from the same values as the
  page.
- Includes can include other fragments. A loop (`a` includes `b` includes `a`)
  is a compile error.
- An include can only go where text goes, not inside an attribute.

### From code: `frag(name, value)`

`frag` renders a fragment with its own values, and returns rendered HTML that
can go into a page's field:

```
frag("entry", {"who": "Ada", "msg": "Hello!"})
```

Given a **list** of maps, it renders the fragment once for each, which is how
templates do loops:

```
return {"entries": frag("entry", data.get("guestbook", []))}
```

```html
<!-- frags/entry.html -->
<li><b>{{who}}</b>: {{msg}}</li>
```

The result of `frag` is a special value, not a string. It goes into a page as
HTML; a plain string never does. A handler can also return it as the
response: see [HTMX](#htmx-updating-part-of-a-page).

---

## Markdown

### Markdown pages

`pages/about.md` is served at `/about`, rendered as HTML. A site can be made
of nothing but `.md` files.

- **The page around it.** If you have `frags/layout/markdown.html`, each
  Markdown page is put into it: it needs a `{{content}}` field, and can have a
  `{{title}}` field, which gets the page's first `#` heading. Without that
  file, Fourfold uses a simple built-in page.
- **Links between `.md` files work** both in Fourfold and when you read the
  files on GitHub or in an editor: `[setup](setup.md#install)` becomes
  `/setup#install`. A link to a page that doesn't exist is a compile warning.
- **Headings get ids** in GitHub's style (`## Hello World` gets
  `id="hello-world"`), so `#hello-world` links work.
- **Markdown pages have no fields and no code.** `{{` in Markdown is just text.

### Markdown fragments

`frags/intro.md` works like an HTML fragment: `{{> intro}}` includes it, and
`frag("intro")` renders it. It takes no values. `intro.md` and `intro.html`
side by side in `frags/` is a compile error.

### Markdown from code: `markdown(text, profile)`

`markdown(text, profile)` renders a string of Markdown, such as a visitor's comment,
and returns rendered HTML, like `frag` does. `text` can also be a list of
lines, as [`file.readLines`](#files-in-data-file-and-rawdata) returns them:

```
showEntry = function(e)
    return {"who": e.who, "msg": markdown(e.msg)}
end function
```

You can render Markdown using either of two profiles:

| | `"comment"` (the default) | `"full"` |
|---|---|---|
| Paragraphs, bold, italic, strikethrough, code, lists, quotes, tables | yes | yes |
| Links | yes, marked `rel="nofollow ugc"` | yes |
| Headings | shown as ordinary paragraphs | yes, with ids |
| Images | shown as a link to the image | yes |

Use the default for anything a visitor wrote: it keeps their text from
showing tracking images, taking over the page with giant headings, or
creating ids that clash with yours. Use `markdown(text, "full")` for text you
trust. `.md` files in `code/` are always rendered in full.

The text is limited to 256 KB.

### What Markdown can't do (on purpose)

- **No raw HTML.** HTML tags in Markdown are shown as text, even in your own
  files. For HTML, write an HTML fragment.
- **Links and images follow the URL rule:** `http:`, `https:`, `mailto:` or
  relative. `[click](javascript:...)` is shown as plain text.
- **No footnotes yet.**

Store Markdown source in `data`, not rendered HTML, and render it when you
show it.

### Styling

Rendered Markdown is wrapped in `<div class="markdown">` (or
`<div class="markdown comment">` for the comment profile), so your CSS can
target `.markdown h2` or `.markdown.comment p` without touching the rest of
the page.

Any page containing rendered Markdown gets a stylesheet link in its `<head>`:
to your `pages/markdown.css` if you have one, otherwise to a built-in default.
Code blocks are `<pre><code class="language-x">`, so client-side syntax
highlighters work.

---

## Code: render and handlers

A page gets code by putting a `.ms` file next to it: `pages/guestbook.html`
pairs with `pages/guestbook.ms`.

A page's `.ms` file **defines functions**; it doesn't do its work at the top
level. Its top level runs at the start of every request, so it should only
define functions and set simple variables. Calls that do real work (`data`,
`frag`, `dropbox`, `wait`) are errors there.

Each request runs in a **fresh MiniScript VM**, thrown away when the request
ends. Globals don't carry over from one request to the next. To keep
something, use [`data`](#persistent-site-wide-data-data) (shared, permanent) or
[`session`](#remembering-visitors-session) (one visitor's).

### `render`: filling the page

`render` is a global function that returns a map of field values for the
page:

```
// pages/index.ms
render = function(req)
    return {"name": "Ada", "visits": data.increment("visits")}
end function
```

It's called for every GET of the page. A page without `render` is served as
is, with empty fields.

**A page made only of code.** A `.ms` file with `render` but no `.html` file
beside it is still a page: `pages/api.ms` is served at `/api`. Its `render`
returns a string, JSON, a redirect, and so on (anything but a map, since
there's no template to fill).

### `get` and `post`: other URLs

Handlers for other URLs go in the `get` and `post` maps, which already exist
when your file runs:

```
// pages/guestbook.ms
get.more = function(req) ... end function     // GET  /guestbook/more
post.sign = function(req) ... end function    // POST /guestbook/sign
```

- The URL is the page's URL plus the handler's name. `post.sign` in
  `pages/guestbook.ms` is `POST /guestbook/sign`.
- In an `index.ms`, handlers live in its folder: `post.sign` in
  `pages/index.ms` is `POST /sign`, and `get.more` in `pages/docs/index.ms` is
  `GET /docs/more`.
- `get.x` and `post.x` can both exist; they share a URL.
- **Only functions in `get` and `post` have URLs.** Helper functions in the
  same file are never reachable from outside.
- Use `get` for anything that only shows something, and `post` for anything
  that changes something. POSTs are protected from forgery
  ([CSRF](#forms-and-csrf-protection)); GETs aren't, and don't need to be.
- A URL that exists but not for the request's method gets a 405.
- A handler whose URL is already a page's (or another handler's) is a compile
  error.

---

## The request: `req`

Every handler, and `render`, receives `req`, a map describing the request. It
is **frozen**: you can read it but not change it.

| Field | Contents |
|---|---|
| `req.method` | `"GET"`, `"POST"`, ... |
| `req.path` | `"/guestbook/sign"` |
| `req.query` | the URL's query parameters, as a map of strings: `?page=2` gives `{"page": "2"}` |
| `req.form` | a submitted form's fields, as a map of strings |
| `req.headers` | the request headers, with **lowercase** names: `req.headers["user-agent"]` |
| `req.htmx` | 1 if HTMX sent the request, else 0 |

Things to know:

- **Everything is a string.** `req.query.page` is `"2"`, not `2`; use `val()`
  to get a number.
- **A missing field isn't there at all**, so `req.form.who` is an error if the
  form didn't send `who`. Check first:
  `if req.form.hasIndex("who") then who = req.form.who`.
- If a name appears more than once (`?tag=a&tag=b`), the first one wins.
- `req.form` is filled from ordinary form posts
  (`application/x-www-form-urlencoded`, what a `<form method="post">` sends).
  File-upload forms (`multipart/form-data`) aren't supported yet.
- Fourfold's own cookies, and its CSRF token, are removed before your code
  sees the request, so logging `req.headers` can never leak a visitor's
  session.
- The request body may be at most 1 MB.

---

## What a handler returns

| Return | Response |
|---|---|
| a map (`render` only) | the page, with its fields filled |
| a string | the string, as plain text (`text/plain`), never HTML |
| `frag(name, value)` | that HTML fragment |
| `markdown(text)` | that rendered Markdown |
| `redirect(url)` | a redirect (303) to `url` |
| `json(value)` | `value` as JSON (`application/json`) |
| `status(code, body)` | any status from 200 to 599, with an optional body |
| `null` (or no `return`) | 204 No Content |
| an error value | an error (500); see [Errors and limits](#errors-and-limits) |

More detail:

- **`redirect(url)`** takes a path (`"/guestbook"`), a relative URL, or an
  `http://` or `https://` URL. URLs starting with `//` are refused, since they
  leave your site while looking like a path. For an HTMX request, it tells
  HTMX to load the new page.
- **`json(value)`** takes numbers, strings, lists, maps and null. Maps'
  keys are written in sorted order.
- **`status(code, body)`**: the body can be a string (sent as plain text),
  `json(...)`, `frag(...)`, or null for none. For example,
  `status(404, frag("notFound"))` or
  `status(422, "Please give a name and a message.")`.
- **A string is never HTML.** The only ways to produce HTML are templates,
  `frag`, `markdown` and `unsafeHTML`.

### `unsafeHTML(text)`

`unsafeHTML("<b>hi</b>")` turns a string into rendered HTML, with no
checking at all. Never pass it anything a visitor could have influenced. It
exists for HTML you build entirely yourself, and its name is easy to search
for when reviewing code.

---

## Shared code: `code/lib`

Modules shared by several pages go in `code/lib/`, and can be `import`ed into any code file:

```
// code/lib/words.ms
greet = function(name)
    return "Hello, " + name + "!"
end function
```

```
// pages/hello.ms
import "words"

render = function(req)
    return {"greeting": words.greet("Ada")}
end function
```

- `import "words"` runs `code/lib/words.ms`, and gives its top-level names to
  your page as the map `words`.
- Only files directly in `code/lib/` can be imported, by name, without `.ms`.
  There's no other way to load code.

---

## Persistent site-wide data: `data`

`data` stores values permanently, in files in the app's `data/` folder. It's
shared by every visitor and every request, and survives restarts and deploys.
Through the `data` module you can get, set, or otherwise manipulate a value
identified by a path-like name.

```
count = data.increment("visits")                  // increment a counter
data.append "guestbook", {"who": who, "msg": msg} // add to a list
entries = data.get("guestbook", [])               // read it back (as a list)
data.set "settings", {"theme": "dark"}            // replace a value
```

### Names

A name (data key) is like a file path without an extension: `"visits"`,
`"polls/lunch"`. Folders are created as needed. Names can't contain `..`, and
no part of a name can start with a dot.

### Values and logs

Each name holds one of two kinds of thing:

- **A value**, stored as `data/name.json`. `set`, `update`, `increment` and
  `compareAndSet` replace it all at once: a reader sees either the old value
  or the new one, never half of each, even if the server crashes mid-write.
- **A log**, stored as `data/name.jsonl` (one entry per line). `append` adds
  an entry, and `get` reads the whole log as a list. Appending is fast and
  never conflicts with other appends, which makes logs right for guestbooks,
  comments, and records of events.

A name is one or the other. Using `append` on a value, or `set` on a log, is
an error.

### What can be stored

Numbers, strings, lists, maps (with string keys) and null, nested up to 64
deep. Functions, `frag` results and other special values can't be stored.
A value or log can be up to 16 MB.

`true` and `false` are 1 and 0 in MiniScript, and they're stored that way.

`data.get` gives you a fresh copy: changing the list or map it returned
changes nothing stored until you store it again.

### `data` functions

| Call | Does |
|---|---|
| `data.get(name, default=null)` | the value, or the log as a list, or `default` if there's nothing by that name |
| `data.set(name, value)` | replaces the value |
| `data.append(name, entry)` | adds an entry to a log |
| `data.update(name, @f, default=null)` | calls `f(old)` (`old` is `default` if there's nothing stored), stores what it returns, and returns that |
| `data.increment(name, by=1)` | adds `by` to a number (a missing one counts as 0), and returns the new number |
| `data.compareAndSet(name, expected, new)` | stores `new` only if the current value equals `expected` (`null` matches "nothing stored"); returns `true` if it did, `false` if not |
| `data.delete(name)` | removes it; returns `true` if there was something to remove, else `false` |
| `data.exists(name)` | `true` if there's a value or log by that name, else `false` |
| `data.names(folder="")` | the names in a folder, sorted; subfolders end in `/` |

### Many visitors at once

Many requests run at the same time, so two of them could read the same value,
change it, and write it back, losing one change. `data` prevents that:

- **Use `increment` for counters**, never `get` and then `set`.
- **Use `update` for any other change based on the old value.** The name is
  locked while your function runs, so no one else's change can slip in
  between:

  ```
  addVote = function(poll)
      if not poll.hasIndex(choice) then poll[choice] = 0
      poll[choice] += 1
      return poll
  end function
  data.update "polls/lunch", @addVote, {}
  ```

  Keep the function quick: other requests that change the same name wait for
  it. If it returns an error, nothing is stored. Inside it, you can update
  other names, but not the same one.
- **Use `compareAndSet` to do something exactly once.** Only one request can
  change `null` to `1`:

  ```
  if data.compareAndSet("seeded", null, 1) then
      // first visit ever: set things up
  end if
  ```
- **`append` needs no care**: concurrent appends never interfere.

These guarantees hold across processes too, should your site ever run in
more than one.

### Errors

When a `data` call fails (a bad name, too much data, a damaged file), it
returns an **error value** rather than stopping your code, as MiniScript's
built-ins do. Test for one with `isa error` if you want to handle it
yourself.

`data` is your app's own folder. You (or an admin) can look at, back up, or
edit the files in `data/` directly; they're plain JSON. Avoid editing them
while the site is busy, since your edit isn't locked.

---

## Files in `data/`: `file` and `RawData`

`data` keeps JSON values and logs. For any other file of your own (a Markdown
page, a CSV export, an image your code made), use `file`. It reads and writes
whole files anywhere in `data/`, as text or as bytes:

```
lines = file.readLines("pages/news.md")       // a list of strings, or null
file.writeLines "exports/votes.csv", rows      // replaces the file
pic = file.loadRaw("images/logo.png")         // RawData, or null
file.saveRaw "images/copy.png", pic
```

The names are those of MiniScript's standard `file` module, so they'll be
familiar if you've used command-line MiniScript or Mini Micro.

### Paths

Paths are relative to `data/`, with `/` between folders: `"notes.txt"`,
`"pages/news.md"`. They can't start with `/`, contain `..`, or have any part
that starts with a dot, so there's no way out of `data/`. Symbolic links are
never followed. Folders are created as needed when you write.

There's no current folder; every path starts at `data/`.

### Whole files, all at once

`file` reads a whole file, and writes a whole file. There's no opening a file
and writing to it bit by bit, and no appending (use
[`data.append`](#persistent-site-wide-data-data) for logs). A write replaces the file all
at once: anyone reading it sees the old file or the new one, never part of
either, even if the server crashes mid-write.

A file can be up to 16 MB, read or written.

### `file` functions

| Call | Does |
|---|---|
| `file.readLines(path)` | the text file's lines, as a list of strings; `null` if there's no such file |
| `file.writeLines(path, lines)` | replaces the file with the lines (a list of strings, or one string), each followed by a line break; returns how many |
| `file.loadRaw(path)` | the file's bytes, as `RawData`; `null` if there's no such file |
| `file.saveRaw(path, data)` | replaces the file with a `RawData`'s bytes |
| `file.exists(path)` | 1 if there's a file or folder there, else 0 |
| `file.info(path)` | a map: `path`, `isDirectory` (1 or 0), `size` (bytes) and `date` (`"yyyy-MM-dd HH:mm:ss"`, in UTC); `null` if there's nothing there |
| `file.children(path="")` | the names of the files and folders in a folder, sorted |
| `file.makedir(path)` | creates a folder (and any folders above it) |
| `file.delete(path)` | removes a file, or an empty folder; 1 if it did, 0 if there was nothing there |
| `file.name(path)` | the last part: `file.name("a/b.txt")` is `"b.txt"` |
| `file.parent(path)` | the folder part: `file.parent("a/b.txt")` is `"a"` |
| `file.child(folder, name)` | the two joined: `file.child("a", "b.txt")` is `"a/b.txt"` |

`file.name`, `file.parent` and `file.child` only work on the strings; they
don't look at the disk. Leaving out the path in `file.children` or
`file.info` means `data/` itself.

### Text and lines

Text files are UTF-8. `readLines` splits a file at its line breaks (`\n`,
`\r\n` or `\r`), without including them; a line break at the very end
doesn't add an empty last line. A file that isn't UTF-8 text, such as an
image, can't be read with `readLines` (it's an error); use `loadRaw`.

To show a Markdown file from `data/`, pass its lines straight to
`markdown`:

```
render = function(req)
    return {"news": markdown(file.readLines("pages/news.md"), "full")}
end function
```

Since it's data, you (or the site's editors, through your code) can change
the file without deploying, and the next request shows the change.

### Binary files: `RawData`

`RawData` is a buffer of bytes. `file.loadRaw` gives you one, and you can
make one with `new RawData`:

```
r = new RawData
r.resize 8                // 8 zero bytes; or: r.len = 8
r.setUint 0, 3000000000
r.setFloat 4, 1.5
file.saveRaw "numbers.dat", r
```

| Member | Is |
|---|---|
| `len` | the size, in bytes; assign to it to resize |
| `resize(bytes=32)` | changes the size, keeping what fits, and adding zeros |
| `littleEndian` | byte order for numbers wider than a byte: 1 (the default) for little-endian, 0 for big-endian |
| `byte(offset)`, `setByte(offset, value)` | an unsigned 8-bit number |
| `sbyte`, `setSbyte` | a signed 8-bit number |
| `ushort`, `setUshort` | an unsigned 16-bit number |
| `short`, `setShort` | a signed 16-bit number |
| `uint`, `setUint` | an unsigned 32-bit number |
| `int`, `setInt` | a signed 32-bit number |
| `float`, `setFloat` | a 32-bit floating-point number |
| `double`, `setDouble` | a 64-bit floating-point number |
| `utf8(offset=0, bytes=-1)` | text from the bytes, up to a zero byte or the end; any bytes that aren't valid UTF-8 come out as `�` |
| `setUtf8(offset, text)` | stores text as UTF-8, as much as fits; returns how many bytes it stored |

A negative offset counts from the end: `r.byte(-1)` is the last byte. An
offset outside the buffer is an error.

A `RawData` can be up to 16 MB, and all the `RawData` one request makes can
total up to 64 MB. `RawData` can't be stored in `session` or `data`, or sent
with `json()`; save it with `file.saveRaw`.

### Many visitors at once

Every write (`writeLines`, `saveRaw`, `delete`) locks its file while it
happens, and `data` locks the same files the same way. So
`file.writeLines("votes.json", ...)` waits for a `data.update("votes", ...)`
in progress, and lands after it, not in the middle. Reads don't wait, and
always see a whole file.

There's no `update` for files: if you read a file, change it, and write it
back, another request's write can come between. For values that change
often, use `data`.

`data.append` doesn't lock, so that appends stay fast. Don't rewrite or
delete a log with `file` while the site might be appending to it; an entry
appended at that moment can be lost.

### `file` and `data` together

`data`'s values and logs are ordinary files in `data/`: the value `"votes"`
is `data/votes.json`, and the log `"guestbook"` is `data/guestbook.jsonl`.
`file` can read them, and write them too (as long as what it writes is
valid JSON, or `data.get` will report the file as damaged).

### Never served by URL

Nothing in `data/` has a URL, now or ever. A visitor sees a file's contents
only if your code reads it and puts it in a page: escaped like any other
value, or rendered with `markdown`.

### Errors

Like `data`, `file` returns an **error value** when a call fails: a bad
path, a file that's too big, a folder where a file was expected. Test for
one with `isa error`. A missing file isn't an error: `readLines`, `loadRaw`
and `info` return `null`.

---

## Remembering visitors: `session`

`session` is a map that belongs to one visitor. Whatever you put in it is
there on their next request:

```
post.sign = function(req)
    ...
    if not session.hasIndex("signed") then session.signed = 0
    session.signed += 1
    return redirect("/")
end function
```

- It's an ordinary map: read and change it however you like. Fourfold saves
  it at the end of the request, if it changed.
- It holds numbers, strings, lists, maps and `null` (just like `data`). Keep it
  small: the limit is **64 KB**.
- A visitor gets a session (and a cookie) only when your code first stores
  something in it. Visitors you never store anything for cost nothing.
- **One request at a time per visitor.** Requests from the same visitor
  (double-clicks, several open tabs) that use `session` take turns, so they
  can't trip over each other.
- **One key is special:** `session.flash` holds a one-time message, removed
  automatically once a page shows it. See
  [One-time messages](#one-time-messages-sessionflash). Don't use it for
  anything else.

### When a session goes away

Treat a session as a convenience that can vanish, not as permanent storage.
It's gone when:

- the visitor hasn't been back for **24 hours**, or it's **7 days** old;
- the visitor clears their cookies or uses another browser;
- the server has to make room (it keeps at most 1,000 sessions per visitor
  address, and 100,000 in all, dropping the oldest first).

Anything that must last belongs in `data`.

### If saving fails

If, at the end of a request, `session` is over 64 KB, holds something that
can't be stored (like a function), or isn't a map anymore, the request fails
with an error and **nothing in the session is saved**. The error says what's
wrong and, if it's too big, which keys are largest.

### Which pages use the session

Loading a session and taking a visitor's turn costs a little time, so
Fourfold does it only for code that needs it. It decides when the app is
compiled: a page or handler uses the session if its `.ms` file (or a lib it
imports) mentions `session`, or if its template has a `{{flash}}` field.

So mentioning `session` anywhere in a page's `.ms` file makes all of that
file's handlers wait their turn, even ones that never touch it. If that
matters for a busy handler, move it to a file that doesn't mention `session`.

---

## One-time messages: `session.flash`

After a form post, a handler usually redirects (so reloading the page doesn't
post again). A flash message carries a short note across that redirect. Put
it in `session.flash`:

```
post.sign = function(req)
    ...
    session.flash = "Thanks for signing, " + who + "!"
    return redirect("/")
end function
```

and give the page the redirect lands on a `{{flash}}` field:

```html
<!-- pages/index.html -->
<p class="flash">{{flash}}</p>
```

`flash` is an ordinary session key with one special behavior: **when a page
with a `{{flash}}` field is shown, `session.flash` is removed.** So the
message appears once, and then it's gone.

- Pages without a `{{flash}}` field leave it waiting.
- Your code can read it, change it, or remove it, like any session value:
  `if session.hasIndex("flash") then ...`. Reading it doesn't use it up; only
  a page showing it does.
- It's escaped like any field.
- With no message waiting, `{{flash}}` is empty. To hide the empty element,
  add `.flash:empty { display: none; }` to your CSS.
- If your `render` returns its own `flash` field, that value is shown
  instead, and `session.flash` is still removed. Use this when your code wants
  to replace a waiting message with one it worked out itself.

**A message can be a map**, taken apart by the template's fields, for
messages that need more than text:

```
session.flash = {"text": "Your changes were saved.", "kind": "ok"}
```

```html
<p class="flash {{flash.kind}}">{{flash.text}}</p>
```

Any field that starts with `flash` (`{{flash}}`, `{{flash.text}}`) counts as
showing it.

Storing a message starts a session if the visitor doesn't have one yet, like
storing anything else in `session`. Put `{{flash}}` only on the pages your
redirects land on, rather than in a header shared by every page: every page
with that field loads the visitor's session, so other pages would load it for
nothing.

---

## Forms and CSRF protection

Forms just work:

```html
<form method="post" action="/guestbook/sign">
  <input name="who"> <textarea name="msg"></textarea>
  <button>Sign</button>
</form>
```

```
post.sign = function(req)
    who = req.form.who
    ...
end function
```

Behind the scenes, Fourfold protects every POST from **cross-site request
forgery** (another site tricking a visitor's browser into posting to yours):

- Every `<form method="post">` in your templates gets a hidden `_csrf` field,
  and every page's `<body>` gets a token for HTMX. You don't write either.
- Every request other than GET, HEAD and OPTIONS must carry a matching token,
  and must not come from another site; otherwise, it's refused with a 403
  ("This form has expired") **before your handler runs**.
- `_csrf` is removed from `req.form`, so your handlers never see it.

What this means for you:

- **Use GET only for requests that change nothing.** GET isn't protected.
- **Forms on other sites can't post to yours**, and neither can scripts or
  services elsewhere. (Receiving webhooks from other services will come with a
  later version.)
- **Forms must be in your templates** to get a token. A form you build with
  JavaScript needs the token too: see below.
- **A visitor who left a form open a long time** may get the 403 when the
  token cookie has gone (say, after clearing cookies). Reloading the page
  fixes it.

### Posting from your own JavaScript

A `fetch` POST needs the token in an `X-CSRF-Token` header. The page's
`<body>` carries it in its `hx-headers` attribute:

```js
const token = JSON.parse(document.body.getAttribute("hx-headers"))["X-CSRF-Token"];
fetch("/guestbook/sign", {
  method: "POST",
  headers: {"X-CSRF-Token": token, "Content-Type": "application/x-www-form-urlencoded"},
  body: new URLSearchParams({who: "Ada", msg: "Hi"}),
});
```

Don't write an `hx-headers` attribute on `<body>` yourself; that's a compile
error, since Fourfold puts its own there.

---

## HTMX: updating part of a page

Fourfold includes [HTMX](https://htmx.org), which lets a page update in place
using HTML attributes instead of JavaScript. Load it in your page's `<head>`:

```html
<script src="/__fourfold/htmx.js"></script>
```

Then a form, link or button can fetch a fragment and put it into the page:

```html
<ul id="entries">{{entries}}</ul>

<form method="post" action="/guestbook/sign"
      hx-post="/guestbook/sign" hx-target="#entries" hx-swap="beforeend">
  <input name="who"> <input name="msg"> <button>Sign</button>
</form>
```

```
post.sign = function(req)
    entry = {"who": req.form.who, "msg": req.form.msg}
    data.append "guestbook", entry
    if req.htmx then return frag("entry", entry)    // just the new <li>
    return redirect("/guestbook")                   // without JavaScript
end function
```

- `req.htmx` tells you whether HTMX sent the request. Return a fragment for
  HTMX, and a redirect (or the whole page) otherwise, and the page works both
  with and without JavaScript.
- The fragment that renders a list in `render` is the same one a handler
  returns for one new item, so its HTML is in one place.
- HTMX requests carry the CSRF token automatically.
- `redirect` in answer to an HTMX request makes HTMX load the new page.
- A response with status 422 is still swapped into the page, so
  `status(422, frag("formErrors", ...))` can show errors in place.

For safety, Fourfold's copy of HTMX is configured not to run JavaScript from
attributes or from swapped-in content. `hx-on...` attributes are compile
errors, and a fragment returned as a response can't contain a `<script>`.

---

## JavaScript and the Content Security Policy

Every page is sent with a Content Security Policy that makes injected script
useless, even if something did slip into a page. In practice:

- **Scripts in your templates work.** Both `<script src="app.js">` and
  inline `<script>` blocks in your pages and fragments run: Fourfold marks each
  one with a fresh random value (a nonce) on every response.
- **Inline event handlers don't.** `onclick="..."` and the like are compile
  errors. Use `addEventListener` in a script, with a `data-` attribute to find
  the element:

  ```html
  <button data-action="hide">Hide</button>
  <script>
    document.addEventListener("click", e => {
      if (e.target.dataset.action === "hide") e.target.hidden = true;
    });
  </script>
  ```
- **`javascript:` URLs don't work**, and `eval` and `new Function` are
  blocked.
- **Scripts from other sites don't load.** Serve your JavaScript files from
  your own app.
- **Inline styles are fine:** `style="..."` and `<style>` work (though fields
  can't go in them).
- **Images may come from any `https:` site**, from your own, or as `data:`
  URLs.
- **Forms may only post to your own site**, and your pages can't be shown in
  another site's frame.

To pass data from your code to a script, use a field where a JavaScript value
goes, or a JSON block:

```html
<script type="application/json" id="stats">{{stats}}</script>
<script>
  const stats = JSON.parse(document.getElementById("stats").textContent);
</script>
```

---

## Output: `print` and the dropbox

`dropbox/` is your app's outbox: it can add to files there, but never read or
change them. It's where you (or an admin) collect what the app reports.

- **`print`** output goes to `dropbox/print.log`, one line each, starting with
  the name of the file that printed it.
- **`dropbox.write(name, text)`** adds `text` as a line to `dropbox/name`:

  ```
  dropbox.write "signups.log", req.form.email
  ```

  Give a plain file name, like `"signups.log"`.

Visitors can't see anything in `dropbox/`.

---

## Errors and limits

### When your code fails

A runtime error in your code, a bad return value, or a broken limit (below)
ends the request with an error:

- The **visitor** sees your `pages/500.html`, or a short plain-text message.
  Never the details.
- The **details** (message, file and line) go to the engine log, in
  `private/state/logs/engine-YYYY-MM-DD.log`. Ask your admin for access to it
  if you don't have it.
- Other requests carry on as normal.

Remember that many MiniScript operations, and Fourfold's own functions,
return an **error value** instead of stopping. If such a value ends up as
your handler's result, that's a 500; if it ends up in a field or in `data`,
you'll usually get an error that names it.

### Limits on each request

| Limit | Default | When exceeded |
|---|---|---|
| Instructions run | 10 million | error |
| Time, including waiting | 10 seconds | error |
| Function call depth | 256 | error |
| One string or list | 16 million characters or items | an error value |
| Request body | 1 MB | 413 |
| A session | 64 KB | error; the session isn't saved |
| A data value or log, or a file | 16 MB | an error value |
| One `RawData` | 16 MB | an error value |
| All of a request's `RawData` | 64 MB | an error value |
| Text given to `markdown()` | 256 KB | an error value |

`wait(seconds)` pauses a request without holding up anyone else, but the time
counts against its 10 seconds.

**Note:** there's no limit yet on how much memory a single request can use.
A script that builds enormous strings in a loop can slow the whole server.
That limit will come; until then, take care with loops that grow data.

### Not available

Some of MiniScript's standard built-ins don't exist or don't work in
Fourfold, since a web request has no use for them or they'd affect other
requests: `exec`, `env`, `exit`, `input`, `reset`, and `gc.collect`. `file`
is Fourfold's own, confined to `data/` (see
[Files in `data/`](#files-in-data-file-and-rawdata)): it has no `open`, no
current folder (`curdir`, `setdir`), and no `move` or `copy`. The built-in types (`string`, `list`, `map`, ...) can't be
extended: `string.shout = ...` is an error.

---

## Deploying changes

`fourfold serve` doesn't pick up changes to `code/` on its own; that would let
a half-uploaded change go live. To deploy:

1. Copy your changed files into the app's `code/` folder.
2. **Touch `code/.deploy`** (for example, `touch code/.deploy`).

Fourfold then compiles the new code and switches to it:

- Requests already running finish on the old version; new ones get the new
  version. Nothing is dropped.
- **If the new code doesn't compile**, the old version keeps serving, and the
  errors (with file and line) go to the engine log. Fix them and touch
  `code/.deploy` again.
- `data/`, sessions and `dropbox/` are untouched by a deploy.

Since anything in `code/` with an error stops the whole deploy, it's best to
check your changes locally first.

---

## Developing on your own machine

`fourfold dev` runs your app on your own computer, for building and testing:

```sh
fourfold dev path/to/myapp
```

Then open `http://localhost:8090/` (or choose another port with `--port N`).
Dev mode is the same engine as `serve`, with extras for the edit-and-try
loop:

- **Live reload.** Save a file in `code/` and open pages reload by themselves,
  within about half a second. A changed stylesheet is swapped in without a
  reload.
- **Errors in the browser.** A compile error shows an overlay on open pages,
  with the file, line and surrounding code, while the last working version
  keeps serving. A runtime error shows the same details on the error page.
  The CSRF 403 page says which check failed.
- **Warnings** (missing or extra fields, broken links, unknown file types) are
  printed in the terminal, and missing and extra fields also appear in a small
  panel on the page.
- **`print`** goes to the terminal.

Dev mode listens only on your own machine (`localhost`), over plain HTTP.
It uses the app's real `data/` folder, so work on a copy if the data matters.

---

## Quick reference

### Files

| Path | Is |
|---|---|
| `code/pages/x.html` | the page at `/x` |
| `code/pages/x.ms` | its code: `render`, `get.*`, `post.*` |
| `code/pages/x.md` | a Markdown page at `/x` |
| `code/pages/dir/index.html` | the page at `/dir/` |
| `code/pages/404.html`, `500.html` | the error pages |
| `code/pages/markdown.css` | styles for rendered Markdown |
| `code/frags/x.html`, `x.md` | the fragment `x` |
| `code/frags/layout/markdown.html` | the page around Markdown pages |
| `code/lib/x.ms` | the module for `import "x"` |
| `code/.deploy` | touch it to deploy |

### Template syntax

| Syntax | Means |
|---|---|
| `{{name}}` | a field |
| `{{user.name}}` | a field inside a map |
| `{{> header}}` | include `frags/header` |
| `{{flash}}` | `session.flash`, if any; showing it removes it |
| `\{{` | a literal `{{` |

### Functions

| Call | Returns |
|---|---|
| `frag(name, value)` | rendered HTML; a list of maps renders it once each |
| `markdown(text, profile="comment")` | rendered HTML (`text` can be a list of lines) |
| `unsafeHTML(text)` | `text`, as HTML, unchecked |
| `redirect(url)` | a redirect response |
| `json(value)` | a JSON response |
| `status(code, body=null)` | a response with any status |
| `import "name"` | runs `code/lib/name.ms`, as the map `name` |
| `wait seconds` | null, after pausing |
| `data.get(name, default=null)` | the stored value or log |
| `data.set(name, value)` | |
| `data.append(name, entry)` | |
| `data.update(name, @f, default=null)` | `f`'s result, now stored |
| `data.increment(name, by=1)` | the new number |
| `data.compareAndSet(name, expected, new)` | 1 if stored, else 0 |
| `data.delete(name)` | 1 if removed, 0 if there was nothing |
| `data.exists(name)` | 1 or 0 |
| `data.names(folder="")` | a sorted list of names |
| `file.readLines(path)` | a list of lines, or null |
| `file.writeLines(path, lines)` | the number of lines |
| `file.loadRaw(path)` | `RawData`, or null |
| `file.saveRaw(path, data)` | null |
| `file.exists(path)` | 1 or 0 |
| `file.info(path)` | `{path, isDirectory, size, date}`, or null |
| `file.children(path="")` | a sorted list of names |
| `file.makedir(path)` | null |
| `file.delete(path)` | 1 if removed, 0 if there was nothing |
| `file.name`, `file.parent`, `file.child` | path strings |
| `new RawData` | an empty buffer of bytes |
| `dropbox.write(name, text)` | null |

### Globals

| Name | Is |
|---|---|
| `req` | the request (frozen) |
| `session` | this visitor's map, saved if changed; `session.flash` is removed once a page shows it |
| `get`, `post` | maps of handlers |
| `render` | the page's render function, if it has one |

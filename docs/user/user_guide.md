# Fourfold User Guide

How to build a web site with Fourfold: pages, templates, MiniScript code,
stored data and sessions. This guide is for the person writing the site. It
assumes Fourfold is already installed and serving your app with
`fourfold serve`; setting that up is covered in the
[admin guide](../admin/admin_guide.md).

*Status: covers what works as of Milestone 9. Features still to come
(background jobs, server push and more) will be added here as they
arrive.*

Markdown has its own reference, [FourFold Flavored Markdown](MARKDOWN.md),
which this guide links to rather than repeats.

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
12. [A database: db.query](#a-database-dbquery)
13. [Changing the data's shape: migrations](#changing-the-datas-shape-migrations)
14. [Remembering visitors: session](#remembering-visitors-session)
15. [One-time messages: session.flash](#one-time-messages-sessionflash)
16. [Signing in: req.user](#signing-in-requser)
17. [Who may see what: access rules](#who-may-see-what-access-rules)
18. [Users and roles: users](#users-and-roles-users)
19. [Configuration: private/config](#configuration-privateconfig)
20. [Forms and CSRF protection](#forms-and-csrf-protection)
21. [Declaring a form: form.x](#declaring-a-form-formx)
22. [File uploads](#file-uploads)
23. [Spam and rate limits: code/limits.ms](#spam-and-rate-limits-codelimitsms)
24. [Calling other servers: http](#calling-other-servers-http)
25. [Secrets: API keys in your code](#secrets-api-keys-in-your-code)
26. [Being called: webhooks](#being-called-webhooks)
27. [Sending email: mail.send](#sending-email-mailsend)
28. [Background jobs: code/jobs](#background-jobs-codejobs)
29. [HTMX: updating part of a page](#htmx-updating-part-of-a-page)
30. [Live updates: publish](#live-updates-publish)
31. [JavaScript and the Content Security Policy](#javascript-and-the-content-security-policy)
32. [Output: print and the dropbox](#output-print-and-the-dropbox)
33. [Errors and limits](#errors-and-limits)
34. [Deploying changes](#deploying-changes)
35. [Developing on your own machine](#developing-on-your-own-machine)
36. [Quick reference](#quick-reference)

---

## The app folder

An app is one folder with four parts:

| Folder | Holds | Your code can |
|---|---|---|
| `code/` | Your site: pages, templates, MiniScript, images, CSS | nothing at run time; it's compiled when deployed |
| `data/` | What the app stores, through [`data`](#persistent-site-wide-data-data) and [`file`](#files-in-data-file-and-rawdata) | read and write it |
| `dropbox/` | The app's output: logs it writes, and `print` output | only add to it |
| `private/` | the app's [configuration](#configuration-privateconfig) (`private/config/`, which you write), and Fourfold's own files: users, sessions, logs, keys (`private/state/`) | nothing |

Inside `code/`:

| Folder | Holds |
|---|---|
| `code/pages/` | Everything with a URL: pages, their code, and static files |
| `code/frags/` | Pieces of HTML (and Markdown) that pages and code put together |
| `code/lib/` | MiniScript modules shared by pages |
| `code/access.ms` | who may see what: [access rules](#who-may-see-what-access-rules) |
| `code/queries/` | the SQL your code can run: [named queries](#a-database-dbquery) |
| `code/migrations/` | changes to the data's shape: [migrations](#changing-the-datas-shape-migrations) |

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

Markdown is a first-class format: a page or a fragment can be a `.md` file,
with fields and code like an HTML one, and code can render a visitor's
Markdown with `markdown(text)`. Every Markdown page is also served *as
Markdown*, for Markdown clients. [FourFold Flavored Markdown](MARKDOWN.md)
describes the Markdown itself completely; this section shows how it fits into
an app.

Markdown comes in three levels:

- **Limited Markdown**, for anything a visitor wrote: `markdown(text)`.
- **Full Markdown**, for text you trust: `markdown(text, "full")`.
- **Dynamic Markdown**, your `.md` files in `code/`: Full Markdown plus
  fields and includes.

### Markdown pages

`pages/about.md` is served at `/about` as HTML, and at `/about.md` as
Markdown. A site can be made of nothing but `.md` files.

- **The page around it.** If you have `frags/layout/markdown.html`, each
  Markdown page's HTML is put into it: it needs a `{{content}}` field, and can
  have a `{{title}}` field, which gets the page's first `#` heading. Without
  that file, Fourfold uses a simple built-in page. The Markdown at
  `/about.md` has no layout.
- **Links between `.md` files work** both in Fourfold and when you read the
  files on GitHub or in an editor: in the HTML, `[setup](setup.md#install)`
  becomes `/setup#install`; in the Markdown, it's kept, and leads to
  `/setup.md`. A link to a page that doesn't exist is a compile warning.
- **Headings get ids** in GitHub's style (`## Hello World` gets
  `id="hello-world"`), so `#hello-world` links work.
- `about.md` and `about.html` side by side is a compile error.

### Markdown pages with code

A Markdown page can have fields, like an HTML page, and `about.md` pairs with
`about.ms`: its `render` fills them, and `get` and `post` work as for any
page.

```
# Visitor counter

This page has been visited **{{visits}}** times.
```

```
render = function(req)
    return {"visits": data.increment("visits")}
end function
```

Values are escaped for where they are, in both the HTML and the Markdown, so
a value can't add a link, a heading or anything else. A few places can't
have fields (a link reference definition, an autolink, inside `[brackets]`
that aren't a link, and others); each is a compile error saying what to do
instead. `{{` in a code span or code block is just text, and `[[` is reserved
(write `\[[`). [Dynamic Markdown](MARKDOWN.md#dynamic-markdown) has the
details.

**Rendered values and includes are blocks.** `{{> name}}`, or a field holding
`frag(...)` or `markdown(...)`, must be a paragraph of its own: alone on its
line, not in a list or quote, with a blank line before and after:

```
# Guestbook

{{entries}}
```

A Markdown page takes only Markdown there: an `.md` fragment, or
`markdown()`. An HTML fragment or `unsafeHTML` is an error.

### Markdown as Markdown

Every Markdown page is also at its name with `.md`: `/about.md`, and
`/docs/index.md` for `/docs/`. It's the same route, with the same access
rules and the same code, and its fields are filled the same way; only the
format differs. A client that reads Markdown, such as a Markdown browser in
Mini Micro, can read the whole site. See
[Markdown as Markdown](MARKDOWN.md#markdown-as-markdown-xmd).

`req.format` is `"markdown"` when the request was for the Markdown, and
`"html"` otherwise. Most pages never need it.

### Markdown fragments

`frags/intro.md` works like an HTML fragment: `{{> intro}}` includes it, and
`frag("intro", value)` renders it, filling its fields; a list renders it once
per item. A Markdown fragment can go in an HTML page or a Markdown page.
`intro.md` and `intro.html` side by side in `frags/` is a compile error.

### Markdown from code: `markdown(text, profile)`

`markdown(text)` renders a string of Markdown, such as a visitor's comment,
and returns rendered HTML, like `frag` does. `text` can also be a list of
lines, as [`file.readLines`](#files-in-data-file-and-rawdata) returns them:

```
showEntry = function(e)
    return {"who": e.who, "msg": markdown(e.msg)}
end function
```

The second argument is the level:

| | `"limited"` (the default) | `"full"` |
|---|---|---|
| Paragraphs, bold, italic, strikethrough, code, lists, quotes, tables | yes | yes |
| Links | yes, marked `rel="nofollow ugc"` | yes |
| Headings | shown as ordinary paragraphs | yes, with ids |
| Images | shown as a link to the image | yes |

Use the default for anything a visitor wrote: it keeps their text from
showing tracking images, taking over the page with giant headings, or
creating ids that clash with yours. Use `markdown(text, "full")` for text you
trust. `{{` in the text is never a field.

In a Markdown page's Markdown (`/x.md`), the value is written out as Markdown,
under the same rules, so a Markdown client gets nothing a browser wouldn't.

The text is limited to 256 KB.

### What Markdown can't do (on purpose)

- **No raw HTML.** HTML tags in Markdown are shown as text, even in your own
  files. For HTML, write an HTML fragment (in an HTML page).
- **Links and images follow the URL rule:** `http:`, `https:`, `mailto:` or
  relative. `[click](javascript:...)` is shown as plain text.
- **No forms.** A Markdown client can read and follow links, but not post.
- **No footnotes yet.**

Store Markdown source in `data`, not rendered HTML, and render it when you
show it.

### Styling

Rendered Markdown is wrapped in `<div class="markdown">` (or
`<div class="markdown limited">` for Limited Markdown), so your CSS can
target `.markdown h2` or `.markdown.limited p` without touching the rest of
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
| `req.form` | a submitted form's fields: as a map of strings, or, with a [form declaration](#declaring-a-form-formx), the declared fields only, checked and typed |
| `req.errors` | with a form declaration: `null` if the form passed its rules, else a map of messages by field name |
| `req.files` | with a form declaration: [uploaded files](#file-uploads), by field name |
| `req.ip` | the visitor's address (from your proxy, if your admin [trusts it](../admin/admin_guide.md#behind-a-reverse-proxy)) |
| `req.headers` | the request headers, with **lowercase** names: `req.headers["user-agent"]` |
| `req.htmx` | 1 if HTMX sent the request, else 0 |
| `req.user` | the [signed-in visitor](#signing-in-requser), or `null` |
| `req.format` | `"html"`, or `"markdown"` when a [Markdown page](#markdown-as-markdown) was asked for as Markdown (`/about.md`) |
| `req.rawBody` | in a [webhook](#being-called-webhooks) only: the body exactly as sent (a string, or `RawData` if it isn't text) |
| `req.json` | in a webhook only: the body parsed, if it's JSON; else `null` |

Things to know:

- **Everything is a string.** `req.query.page` is `"2"`, not `2`; use `val()`
  to get a number.
- **A missing field isn't there at all**, so `req.form.who` is an error if the
  form didn't send `who`. Check first:
  `if req.form.hasIndex("who") then who = req.form.who`.
- If a name appears more than once (`?tag=a&tag=b`), the first one wins.
- `req.form` is filled from ordinary form posts
  (`application/x-www-form-urlencoded`) and file-upload forms
  (`multipart/form-data`). Files reach your code only through a
  [form declaration](#file-uploads).
- Fourfold's own cookies, and its CSRF token, are removed before your code
  sees the request, so logging `req.headers` can never leak a visitor's
  session.
- The request body may be at most 1 MB, plus whatever a form declares for its
  uploads.

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
| `reshow(fragName, extraErrors)` | a [declared form](#showing-errors-and-keeping-input) again, with its errors |
| `file.send(path, downloadName)` | a file from `data/` ([safely](#sending-files-back-filesend)) |
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

## A database: `db.query`

For data that outgrows a few JSON files (many rows, searching and sorting,
several kinds of record that refer to each other), use a SQL database. Out of
the box it's **SQLite**: a single file, `data/.fourfold/app.sqlite`, made the
first time it's needed, with real transactions and no server to run. A host
can switch an app to **Postgres** in its configuration
([admin guide](../admin/admin_guide.md#a-postgres-database)); your code stays
the same.

Your code runs only SQL you've written in **`code/queries/`**, one statement
per file, by the file's name:

```
-- code/queries/findUser.sql
SELECT id, name FROM users WHERE email = {{0}}
```

```
rows = db.query("findUser", [email])
if rows isa error then return rows
if rows.len == 0 then return "No such user"
name = rows[0].name
```

There's no way to run SQL from a string. Values go in only through
placeholders, and are always passed to the database separately from the SQL,
never pasted into it. So no value, whatever a visitor typed, can change what
a query does: SQL injection can't happen.

### Placeholders

A placeholder is `{{key}}`, like a template field, where the key is a name or
an index:

```
-- code/queries/addNote.sql
INSERT INTO notes (title, body, author) VALUES ({{title}}, {{body}}, {{author}})
RETURNING id
```

- **Names** (`{{title}}`) take their values from a map:
  `db.query("addNote", {"title": t, "body": b, "author": req.user.id})`.
  Extra keys are ignored, so you can pass `req.form`, or a row you already
  have.
- **Indexes** (`{{0}}`, `{{1}}`, counting from 0) take theirs from a list,
  `db.query("findUser", [email])`, or from a map with the keys `0` or `"0"`.
- A key can appear more than once: `WHERE owner = {{user}} OR editor = {{user}}`.
- A value that's missing is an error; so is a list for a query with named
  placeholders.

A placeholder stands for a value only, never for SQL: not for a table or
column name, and not inside quotes. `'{{name}}'` is an error when the app
compiles ("placeholders can't go inside quotes"); write `{{name}}`, without
quotes, and the database treats the value as a string anyway.

### Choosing a column: name slots

Sometimes the visitor chooses part of the SQL itself, such as which column
to sort by. A **name slot** lists the names it may be, and the value picks
one:

```
-- code/queries/listNotes.sql
SELECT id, title, created, stars FROM notes
WHERE owner = {{user}}
ORDER BY {{sort: title, created, stars}} {{dir: asc, desc}}
```

```
db.query("listNotes", {"user": req.user.id, "sort": req.query.sort, "dir": "desc"})
```

The chosen name goes into the SQL as you wrote it in the list, as if you'd
typed it there. A value that isn't on the list (matched ignoring case) is an
error, `{{sort}} must be title, created or stars`, so nothing a visitor
sends can become SQL. Each name must be a plain name (letters, digits and
`_`), or two joined by a dot (`notes.title`); `asc` and `desc` are names
too. A slot's key follows the same rules as a placeholder's, and can't also
be used as a value in the same query.

### What comes back

`db.query` returns the statement's rows, as a list of maps by column name.
A statement that returns no rows (most `INSERT`, `UPDATE` and `DELETE`s)
returns an empty list. To get a new row's id, add `RETURNING id`, which
both databases understand.

| In the database | In MiniScript |
|---|---|
| NULL | `null` |
| integers and decimals | numbers |
| text | strings |
| booleans (Postgres) | 1 and 0 |
| `json` and `jsonb` (Postgres) | maps and lists |
| binary data (BLOB, `bytea`) | an error: store text instead |

Values going in: numbers (whole ones as integers), strings, `null`, and lists
and maps, which are stored as JSON text.

### Errors and limits

A failed query (a constraint broken, a misspelled column) returns an **error
value** with the database's own message, such as
`db.query("addUser"): UNIQUE constraint failed: users.email`. Check with
`isa error` where you can do something about it; otherwise return it, and
the visitor sees the error page.

A query may run as long as the request's time budget (`budget.seconds`), and
return at most 10,000 rows; both can be changed in
[the configuration](#configuration-privateconfig) (`db.queryTimeoutSeconds`,
`db.maxRows`). Use `LIMIT` for long lists.

Each `db.query` is its own transaction. (Several queries as one transaction,
from a request, aren't available yet; a migration's are always one.)

### SQLite and Postgres: variants

The two databases differ in places: how a table gets an auto-numbered key,
types, date functions, and more. A query (or migration) that must differ can
have a **variant** for each database, beside the plain file:

```
code/queries/
  findUser.sql             used by both
  recentPosts.sqlite.sql   used on SQLite
  recentPosts.pg.sql       used on Postgres
```

A variant replaces the plain file on its database; a variant for the other
database is ignored. Every variant of a query must use the same placeholder
keys (their order can differ), so your code passes the same values either
way. Most apps need few variants, or none, if they keep to SQL both
understand:

- **Tables:** on SQLite, write `STRICT` after a table's definition, so its
  columns hold only their declared types, as Postgres's do. For an
  auto-numbered key, SQLite's `id INTEGER PRIMARY KEY` and Postgres's
  `id SERIAL PRIMARY KEY` differ: that's the usual first variant.
- **Times:** store them as ISO text (`"2026-11-05T14:02:12Z"`) or as numbers
  (seconds), and compare those. Each database's "now" differs
  (`datetime('now')` and `now()`).
- **True and false:** use an `INTEGER` column holding 0 and 1. (Postgres
  `BOOLEAN` columns come back as 1 and 0 too, but SQLite has none.)
- **Text matching:** `LIKE` (SQLite's ignores case for ASCII, Postgres's
  doesn't; `lower(x) LIKE lower({{q}})` behaves the same on both). Avoid
  Postgres's `ILIKE`.
- **Upserts and new ids:** `INSERT ... ON CONFLICT (...) DO UPDATE` and
  `RETURNING` work on both.
- **A value Postgres can't place:** `SELECT {{0}}` alone leaves Postgres
  unsure of the value's type; write `CAST({{0}} AS TEXT)` (or `{{0}}::text`
  in a `.pg.sql` variant).

### Using a database: a first table

Tables are made by [migrations](#changing-the-datas-shape-migrations), not
by your pages. A first one:

```
-- code/migrations/001_create_notes.sql
CREATE TABLE notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT) STRICT;
```

`fourfold dev` applies it as soon as you save; `examples/tasks` is a
complete small app.

---

## Changing the data's shape: migrations

A site's data outlives its first design: tables gain columns, data files
change shape. A **migration** is a numbered file in `code/migrations/` that
makes one such change. Migrations run in order, once each, and the record of
which ones have run is kept with the data, so a fresh install (or a restore
from a backup) knows exactly what to run.

```
code/migrations/
  001_create_notes.sql
  002_add_stars.sql
  003_backfill_stars/
    migrate.ms
    all.sql
    setStars.sql
  004_split_guestbook.ms
```

The number comes first (`001`, or `1`; gaps are fine); the rest of the name
is up to you. There are three kinds:

- **A `.sql` file** runs as it is, against the database: any number of
  statements, and no placeholders (there are no values to give it). It can
  have variants (`001_create_notes.pg.sql`), as queries can.
- **A folder** is for changes that need logic. Its `migrate.ms` defines
  `migrate = function()`, which runs with `db`, `data` and `file`. Its
  `db.query` reaches only the `.sql` files in its own folder, and it can
  `import` only the other `.ms` files there (copy a helper in from
  `code/lib/` if you need one). That's on purpose: a migration may run years
  from now, on a fresh install, and it must do then exactly what it does
  today, whatever has happened to the rest of the code.

  ```
  // code/migrations/003_backfill_stars/migrate.ms
  migrate = function
      for note in db.query("all")
          db.query "setStars", {"id": note.id, "stars": note.title.len % 5}
      end for
  end function
  ```
- **A single `.ms` file** is for data files only (it can't use `db`, or
  import anything): reshaping what's in `data/`, say.

  ```
  // code/migrations/004_split_guestbook.ms
  migrate = function
      for entry in data.get("guestbook", [])
          data.append "guestbook/" + entry.year, entry
      end for
      data.delete "guestbook"
  end function
  ```

`migrate` returning an error value fails the migration. Migrations can't
send mail, or use sessions or users: they change data, and nothing else.
Each may run for 10 minutes (`deploy.migrationBudgetSeconds`).

### How they're applied

Before applying new migrations, Fourfold holds new requests (they wait
rather than fail), lets those already running finish, and **takes a backup**.
Then it runs the migrations; all their database work is one transaction.

- **If they all succeed**, the new version of the code starts serving, and
  the held requests go on to it. The pause is usually under a second.
- **If one fails**, nothing is changed: the database work is undone, the data
  files are put back from the backup, and the old version goes on serving.
  The error says which migration failed, and why.

Requests that would wait longer than 30 seconds (`deploy.migrationWaitSeconds`)
get a 503 page instead: `code/pages/503.html` if you have one ("down for
maintenance; back shortly"), or a plain one.

**In dev mode**, migrations are applied as soon as you save. **In
production**, they wait for you: see [Deploying changes](#deploying-changes).

### Rules

- **An applied migration can't change.** Editing one afterward is a compile
  error ("002_add_stars.sql was changed after it was applied; write a new
  migration instead"). In dev mode, "redo the last migration" (below) is how
  you fix one you're still writing.
- **There are no "down" migrations.** To undo one, restore the backup taken
  before it, or write a new migration that reverses it.
- **A new migration numbered below one already applied** (say, from merging
  two lines of work) is a compile error: renumber it.
- **Code older than the data is refused.** If the data has migration 7 and
  the code being deployed stops at 5, it would run against a shape it doesn't
  know, so it doesn't run. (The admin guide has the emergency override.)
- Sessions aren't migrated: a `session` written by old code keeps its old
  shape, so code should tolerate that. (Your host can clear anonymous
  sessions after migrations: `deploy.clearAnonymousSessions`.)

### Sample data: `code/seed.ms`

In dev mode, `code/seed.ms`, if you have one, runs after the migrations on a
fresh database (and after "reset dev data"), to fill it with sample content.
It's an ordinary script with the app's `db.query`, `data`, `file` and
`code/lib/`. It never runs in production.

```
// code/seed.ms
for title in ["Buy milk", "Write the report"]
    db.query "addNote", {"title": title, "body": ""}
end for
```

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

- the visitor hasn't been back for **24 hours**, or it's **7 days** old (for
  a signed-in visitor, **14 days** and **30 days**);
- the visitor signs out;
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

## Signing in: `req.user`

Fourfold signs visitors in for you. Your code never sees a password, a
sign-in token or a session cookie; it sees `req.user`:

```
render = function(req)
    if req.user == null then return {"greeting": "Hello, visitor."}
    return {"greeting": "Hello, " + req.user.name + "."}
end function
```

`req.user` is `null` for a visitor who isn't signed in, or a frozen map:

| Field | Contents |
|---|---|
| `id` | a stable, opaque string: use it as the key for the user's data |
| `name` | a display name (at first, the part of the email address before the `@`) |
| `email` | the email address they signed in with (lowercase) |
| `roles` | a list of role names, like `["admin"]` |

Fourfold keeps only this; anything else about a user (a profile, settings,
their posts) is your app's data, kept with `data` and keyed by `req.user.id`.

### The sign-in pages

The pages live at reserved URLs, and you link to them:

| URL | Is |
|---|---|
| `/__auth/login?next=/somewhere` | the sign-in form; afterward, the visitor goes to `next` |
| `/__auth/logout` | a page with "Sign out" and "Sign out on every device" buttons |

To sign out from your own page, post to `/__auth/logout` (a form in your
template gets its CSRF token like any other):

```html
<form method="post" action="/__auth/logout"><button>Sign out</button></form>
```

Add a hidden `everywhere` field with the value `1` to sign out on every
device, and a `next` field to say where to go afterward (the home page if you
don't).

**Signing in is by email:** the visitor types their address, and Fourfold
emails them a link. The link works once, for 15 minutes; opening it shows a
"Sign in" button, and pressing it signs them in. There's no password, and
nothing to sign up for: an address's first sign-in creates its user. (To
allow only users you've chosen, see `signUp` under
[Configuration](#configuration-privateconfig).) Sending email needs setting
up, also under Configuration; in dev mode, the email is printed in the
terminal instead.

What Fourfold does along the way:

- **The answer to "email me a link" is the same** whether or not the address
  can sign in, so the form doesn't tell anyone who has an account.
- **Links are limited:** three an hour for one address, and ten an hour from
  one visitor's address, so the form can't be used to flood an inbox.
- **Signing in starts a new session**, with a new ID, so an ID someone
  planted beforehand is useless. Whatever the visitor had in `session` before
  signing in (a shopping cart, say) is kept.
- **Signing out deletes the session** on the server, not just the cookie,
  along with everything in it.
- Sign-ins, sign-outs and refused links are written to the engine log.

### Making the pages look like your site

The sign-in pages are plain. To wrap them in your site's design, add
`code/frags/auth/layout.html`, a whole page with a `{{content}}` field (and
`{{title}}`, if you like):

```html
<!DOCTYPE html>
<html lang="en">
<head><title>{{title}} · My Site</title><link rel="stylesheet" href="/style.css"></head>
<body>
  {{> header}}
  <main>{{content}}</main>
</body>
</html>
```

Fourfold puts its form (or message) in `{{content}}`, so the design is yours
but the form isn't: a template can't drop its protection or send it
somewhere else.

---

## Who may see what: access rules

Pages are public unless a rule says otherwise. A rule names who may use a
URL:

| Value | Means |
|---|---|
| `"login"` | anyone signed in |
| `"admin"` (any role name) | signed in, with that role |
| `["admin", "moderator"]` | signed in, with **any** of these roles |
| `"public"` | anyone; adds nothing to a page (rules only tighten), but declares a [live channel](#live-updates-publish) |

Rules go in two places.

**`code/access.ms`**, for whole parts of the site:

```
rules = {}
rules["/members"] = "login"
rules["/admin"] = "admin"
rules["/forum/mod"] = ["admin", "moderator"]
rules["/board/post"] = "login"
```

A rule covers its path **and everything below it**: `"/members"` covers
`/members/`, `/members/handbook`, and `/members/photos/team.jpg`, but not
`/membership`. That includes static files and Markdown pages, so a members'
folder keeps its images and videos to members too. A rule can also name one
handler (`/board/post` above), leaving the rest of its page public. There are
no wildcards (`/users/*/edit`); a rule is a plain path, written without a
trailing slash.

**`access =` in a page's `.ms` file**, for that page and every handler in it:

```
// pages/admin/index.ms
access = "admin"
```

How they combine:

- **Every rule that applies must be met.** If `access.ms` says `"login"` for
  `/team` and `pages/team/pay.ms` says `"accountant"`, a visitor needs to be
  signed in *and* an accountant. A rule can only make a page harder to reach,
  never easier.
- So there's no public page inside a protected folder: to make
  `/admin/help` public, move it out of `/admin`.
- `access.ms` and `access =` are read when the app is compiled, as data:
  they can build strings and lists, but can't use `data`, `session`, files or
  `import`. A change takes effect with the next deploy, like any code.
- A rule in `access.ms` that covers nothing gets a warning when the app
  compiles, usually because a folder was renamed. A mistake in a rule (a bad
  path or value) is a compile error.

What visitors see when a rule keeps them out:

| Who | Asking for a page (GET) | Anything else (a POST, say) |
|---|---|---|
| not signed in | sent to sign in, then back to the page | 401 |
| signed in, without the role | 403 | 403 |

Your handlers never run for them. Pages behind a rule are sent with
`Cache-Control: private, no-store`, so shared caches never hand them to
someone else. (So are pages shown to a signed-in visitor, since they may
mention who's asking.)

Finer checks, like "only the author may delete this post", are your code's:
compare `req.user.id` with what you stored.

In dev mode, **the access report** at `/__dev/access` lists every URL your app
serves, who may use it, and which rules said so.

---

## Users and roles: `users`

Roles are names you choose: `"admin"`, `"moderator"`, `"editor"`. A user's
roles are in `req.user.roles`, and your code can change them with `users`:

| Call | Does |
|---|---|
| `users.get(id)` | the user's map (like `req.user`), or `null` |
| `users.find(email)` | the same, by email address |
| `users.grant(id, role)` | gives the user the role |
| `users.revoke(id, role)` | takes it away |

```
post.promote = function(req)
    u = users.find(req.form.email)
    if u == null then return status(404, "No such user; they need to sign in once first.")
    users.grant u.id, "moderator"
    return redirect("/admin/")
end function
```

- A change applies from the user's next request: roles are looked up on every
  request, never remembered in the session.
- A role name starts with a letter, then letters, digits, `_` or `-`.
  `"login"` and `"public"` can't be roles.
- Granting a role a user has, or revoking one they don't, is fine. A user that
  doesn't exist is an error.
- Your code is trusted to do this; guard the pages that call it with an access
  rule (`"admin"`, say).
- The first admins come from the configuration (`auth.admins`): they get the
  `"admin"` role when they sign in.

---

## Configuration: `private/config`

An app needs no configuration to start: every setting has a default. When it
does need some, it goes in `private/config/app.ms`, a MiniScript file read as
data (no `data`, files or `import`), which sets a map called `config`:

```
// private/config/app.ms
config = {}
config.domain = "club.example.com"
config.auth = {"admins": ["you@example.com"], "signUp": "open"}
config.mail = {"transport": "smtp", "from": "Club <noreply@example.com>",
    "smtpHost": "smtp.example.com", "smtpUser": "noreply@example.com",
    "passwordSecret": "smtpPassword"}
```

`fourfold dev` creates `private/config/` with a commented `app.ms` and
`secrets.ms` to start from. Setting up mail, the domain and the first admin
on a server is covered in the [admin guide](../admin/admin_guide.md#mail-for-sign-in).

| Setting | Default | Is |
|---|---|---|
| `domain` | none | the site's host name; sign-in links point to it. Needed by `fourfold serve` to send them |
| `auth.admins` | `[]` | email addresses given the `"admin"` role when they sign in |
| `auth.signUp` | `"open"` | `"open"`: anyone with an email address can sign in. `"closed"`: only users who already exist, and `admins` |
| `auth.methods` | `["magicLink"]` | sign-in methods; `[]` turns sign-in off |
| `auth.linkMinutes` | 15 | how long a sign-in link works |
| `auth.fakeLogin` | on in dev | the "sign in as..." test users; can't be on with `fourfold serve` |
| `mail.transport` | `"log"` in dev, else `"none"` | `"smtp"`, `"postmark"`, `"log"` (written to the engine log, and in dev to the terminal), or `"none"` |
| `mail.from` | | the sender: `"noreply@example.com"` or `"Name <noreply@example.com>"` |
| `mail.smtpHost`, `smtpPort` | port 587 | your mail server |
| `mail.smtpSecurity` | `"starttls"` | `"starttls"` (required), `"tls"` (port 465), or `"none"` (only for a server on the same machine) |
| `mail.smtpUser`, `passwordSecret` | | the login, and the name of the secret holding the password |
| `mail.postmarkTokenSecret`, `postmarkStream` | stream `"outbound"` | for Postmark: the secret holding the server token |
| `budget.instructions`, `budget.seconds` | 10,000,000; 10 | each request's limits |
| `session.capKB` | 64 | the largest `session` |
| `session.anonIdleHours`, `anonMaxDays` | 24; 7 | when an anonymous session ends |
| `session.userIdleDays`, `userMaxDays` | 14; 30 | when a signed-in session ends |
| `session.anonPerAddress`, `anonTotal` | 1,000; 100,000 | the most anonymous sessions per visitor address, and in all |
| `limits.anonymous` | `"60/minute"` | requests other than GET per visitor address, for visitors not signed in (a rate or a list) |
| `limits.signedIn` | `"300/minute"` | the same per user, for signed-in visitors |
| `limits.challengeBits` | 16 (4 in dev) | how hard a proof-of-work puzzle is; each bit doubles the work |
| `proxy.trust` | `[]` | addresses (or ranges, `"10.0.0.0/8"`) of a reverse proxy whose `X-Forwarded-For` is believed; see the [admin guide](../admin/admin_guide.md#behind-a-reverse-proxy) |
| `db.url` | none: SQLite | a Postgres database, `"postgres://host/dbname"`; see the [admin guide](../admin/admin_guide.md#a-postgres-database) |
| `db.passwordSecret`, `db.schema` | | Postgres: the secret holding the password, and a schema to work in |
| `db.pool` | 4 | connections to the database |
| `db.queryTimeoutSeconds` | `budget.seconds` | how long one query may run |
| `db.maxRows` | 10,000 | the most rows one query may return |
| `outbound.enabled` | `true` | `false` turns off [`http`](#calling-other-servers-http) whatever `code/outbound.ms` says |
| `outbound.allow` | none | a cap: hosts (as in `outbound.ms`) your code's list must also fit within |
| `outbound.allowAddresses` | `[]` | internal addresses (or ranges) `http` may reach, which are otherwise always refused |
| `outbound.perMinute` | 120 | the most outbound requests a minute, for the whole app |
| `outbound.maxResponseKB` | 5,120 | the largest response (after decompression) |
| `outbound.timeoutSeconds` | 10 | the longest one request may take, redirects included (and never past the request's own budget) |
| `mail.appMail` | `true` | `false` turns off [`mail.send`](#sending-email-mailsend) |
| `mail.appPerHour` | 100 | the most messages `mail.send` may send an hour, for the whole app |
| `jobs.timeZone` | `"UTC"` | the [schedule's](#on-a-schedule-codeschedulems) time zone: an IANA name like `"America/Denver"` |
| `jobs.workers` | 2 | the most jobs running at once |
| `jobs.instructions`, `jobs.seconds` | 100,000,000; 60 | each job run's limits |
| `jobs.maxAttempts` | 5 | tries before a failing job is dead |
| `deploy.migrationWaitSeconds` | 30 | how long requests wait while migrations run, before a 503 |
| `deploy.migrationBudgetSeconds` | 600 | how long one migration may run |
| `deploy.clearAnonymousSessions` | `false` | sign out every anonymous visitor after migrations |
| `backups.folder` | `private/state/backups/` | where backups go: a full path, outside the app's other folders |
| `backups.everyHours` | 0 (none) | take a backup on a schedule |
| `backups.keep` | 10 | how many automatic backups (before migrations, and scheduled) to keep |

**Secrets** (passwords and API keys) go in `private/config/secrets.ms`, so
`app.ms` can be shared or kept in version control while `secrets.ms` isn't:

```
// private/config/secrets.ms
secrets = {}
secrets.smtpPassword = {"value": "..."}
```

`app.ms` names secrets; it never holds them. A secret with no `hosts` (like
this one) is for Fourfold's own use and never visible to your code. Secrets
your code uses, for API keys and webhook signatures, are in
[Secrets: API keys in your code](#secrets-api-keys-in-your-code).

How changes are handled:

- **A changed config is read again within a second or so**, without a deploy.
  (A changed `domain` takes effect after a restart.)
- **A misspelled or unknown setting is an error**, not ignored, so a typo
  can't leave something at its default.
- **A config with an error isn't used.** The previous one stays, and the error
  goes to the terminal (dev) or the engine log. `fourfold serve` won't start
  with one; `fourfold dev` starts with the defaults and picks up the fix.
- Your host may set **ceilings** (in its own server config) on budgets, jobs and
  session limits; a value over one is lowered to it, with a warning. (See
  [Server limits](../admin/admin_guide.md#server-limits---server-config) in
  the admin guide.)

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
- `_csrf` is removed from `req.form`, so your handlers never see it. So are
  the timing token (`_t`) and puzzle answer (`_pow`) described in
  [Spam and rate limits](#spam-and-rate-limits-codelimitsms).

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

## Declaring a form: `form.x`

Everything in a submitted form is untrusted. Rather than check each field by
hand, declare the fields a handler accepts, and Fourfold checks them before
the handler runs. `form.sign` describes what `post.sign` accepts:

```
// code/pages/guestbook.ms
form.sign = {
    "who":   {"type": "text", "required": true, "max": 40,
              "messages": {"required": "Please sign your name."}},
    "msg":   {"type": "multiline", "required": true, "max": 2000, "maxLinks": 2},
    "mood":  {"type": "choice", "options": ["happy", "meh", "grumpy"]},
    "shout": {"type": "bool"},
}

post.sign = function(req)
    if req.errors then return reshow("signForm")
    data.append "guestbook", {"who": req.form.who, "msg": req.form.msg}
    return redirect("/guestbook")
end function
```

Like `access =`, a form declaration is read **when the app compiles**, so it
must be plain data, and a mistake in it (an unknown type, a misspelled rule)
is a compile error. `form.x` must go with a `post.x` in the same file.

| Type | Accepts | In `req.form` |
|---|---|---|
| `text` | one line (a newline is an error) | a string |
| `multiline` | any text; line endings become `\n` | a string |
| `email` | an email address | a string |
| `url` | an `http://` or `https://` address | a string |
| `number` | a decimal number; no `Infinity`, `NaN` or `1e400` | a number, or null |
| `int` | a whole number | a number, or null |
| `bool` | a checkbox: there means true | 1 or 0 |
| `choice` | one of `options` (radio buttons, a `<select>`) | a string, or null |
| `choices` | any of `options` (several checkboxes) | a list of strings |
| `date` | `YYYY-MM-DD`, as `<input type="date">` sends | a string, or null |
| `file` | an upload; see [File uploads](#file-uploads) | (in `req.files`) |

| Rule | For | Means |
|---|---|---|
| `required` | any | must be filled in (a checkbox: checked; `choices`: at least one) |
| `min`, `max` | text types; `number`, `int` | the length, in characters; or the value |
| `maxLinks` | text types | at most this many web addresses (a cheap spam filter) |
| `options` | `choice`, `choices` | the allowed values |
| `maxKB`, `types` | `file` | see [File uploads](#file-uploads) |
| `messages` | any | your own message per rule: `{"required": "...", "max": "..."}`; `"type"` for a value of the wrong kind |

What happens before your handler runs:

- **Text is cleaned up:** surrounding spaces trimmed, Unicode normalized
  (NFC, so an accented letter typed two ways compares equal), and control
  characters removed (tabs and newlines are kept in `multiline`). A field of
  only spaces counts as empty.
- **Values are converted**, so `req.form.age` is a number.
- **Undeclared fields are dropped.** A visitor who adds `isAdmin=1` to the
  form gets nothing for it. So is a `choice` that isn't one of the options.
- **Every declared field is there**, so you needn't use `hasIndex`. A text
  field that's empty is `""`; a number, date or choice that's empty or
  invalid is `null`; a field that broke a rule is `null`.
- **The errors are in `req.errors`**: `null` if all is well, else a map from
  field name to message. Your handler runs either way, since only it knows
  what to show next (usually `reshow`).

A handler with no declaration still gets `req.form`, as raw strings. The
dev tools' access report marks those handlers, since a declaration is safer.

### Showing errors and keeping input

The form itself goes in a fragment, so it can be shown in the page and on
its own. `formState("sign")` gives it what it needs: each field's value, and
`errors`.

```html
<!-- code/frags/signForm.html -->
<form id="signForm" method="post" action="/guestbook/sign"
      hx-post="/guestbook/sign" hx-target="#entries" hx-swap="beforeend">
  <p class="error">{{errors.form}}</p>
  <input name="who" value="{{who}}"> <span class="error">{{errors.who}}</span>
  <textarea name="msg">{{msg}}</textarea> <span class="error">{{errors.msg}}</span>
  <label><input type="radio" name="mood" value="happy" checked="{{mood.happy}}"> happy</label>
  <label><input type="checkbox" name="shout" checked="{{shout}}"> Shout</label>
  <button>Sign</button>
</form>
```

```
render = function(req)
    return {"signForm": frag("signForm", formState("sign")), ...}
end function

post.sign = function(req)
    if req.errors then return reshow("signForm")
    if isBanned(req.form.who) then return reshow("signForm", {"who": "Sorry, not allowed."})
    ...
end function
```

- **`formState(name)`** is normally empty: `""` for each text field, 0 for a
  checkbox, and for a `choice` or `choices` a map from each option to 1 or 0,
  so `checked="{{mood.happy}}"` and `selected="{{mood.happy}}"` re-check what
  the visitor picked. `errors` is an empty map. Right after a failed
  submission, it holds what the visitor typed and what went wrong.
- **`reshow(fragName, extraErrors)`** sends the form back, either way it was
  sent:
  - **with HTMX**, it renders the fragment and has HTMX replace the form
    itself (status 422). That's why the fragment's first element needs an
    `id`.
  - **without JavaScript**, it keeps the input and errors in the visitor's
    session, for one showing, and redirects to the handler's page
    (`/guestbook` for `pages/guestbook.ms`), whose `formState` picks them up.
- `extraErrors` adds checks only your code can make: a map from field names
  to messages, with `"form"` for an error about the whole form.
- Values come back through ordinary fields, so they're escaped like any other.

### Forms checked against your templates

Since both are read when the app compiles, Fourfold matches each
`<form>` to the declaration it posts to (by its `hx-post` or `action`):

- An input whose name isn't declared (usually a typo) is a warning, as is a
  declared field with no input.
- The declaration's rules are added to the inputs as **browser-side
  checks**, where you haven't written them: `required`, `maxlength`,
  `minlength`, `min`, `max`, `step` and `accept`. Visitors get instant
  feedback; the server still checks everything.

---

## File uploads

Declare a `file` field, and give the form `enctype="multipart/form-data"`:

```
form.upload = {
    "photo":   {"type": "file", "required": true, "maxKB": 512, "types": ["image/png", "image/jpeg"]},
    "caption": {"type": "text", "max": 80},
}

post.upload = function(req)
    if req.errors then return reshow("uploadForm")
    photo = req.files.photo
    ext = ".png"
    if photo.type == "image/jpeg" then ext = ".jpg"
    name = str(data.increment("photoCount")) + ext
    file.makedir "photos"
    file.saveRaw "photos/" + name, photo.data
    return redirect("/photos")
end function
```

```html
<form id="uploadForm" method="post" action="/photos/upload" enctype="multipart/form-data">
  <input type="file" name="photo"> <span class="error">{{errors.photo}}</span>
  ...
```

- **Uploads are checked by their content**, not by their name or the type
  the browser claims: a "PNG" must start like a PNG. The types Fourfold can
  check are `image/png`, `image/jpeg`, `image/gif`, `image/webp`, `image/*`
  (any of those four), `application/pdf`, `application/zip` and `text/plain`.
  `types` is required.
- **`maxKB`** is the largest file, in KB (default 1024, at most 16384).
- **`req.files.photo`** is a map: `name` (the visitor's file name, with any
  folders and control characters removed), `type` (as the content showed
  it), `size` in bytes, and `data`, the bytes as [`RawData`](#binary-files-rawdata).
  A field with no file isn't in `req.files`.
- **Saving is up to you**, with `file.saveRaw`. Prefer names you choose
  over the visitor's: theirs may be anything.

### Sending files back: `file.send`

Nothing in `data/` is ever served by URL. To send a file back, a handler
returns `file.send(path)`:

```
get.show = function(req)
    return file.send("photos/" + req.query.name)
end function
```

- The type comes from the file's extension, from a short list. Images
  (`.png`, `.jpg`, `.gif`, `.webp`, `.avif`), plain text, audio and video are
  shown in the browser; everything else (PDFs, ZIPs, CSV, ...) is
  downloaded; and anything that could hold script, like `.html` and `.svg`,
  is downloaded as plain bytes, never as a page. So an uploaded file can't
  become a page on your site.
- `file.send(path, "report.pdf")` always downloads, under that name.
- The path is checked like any `file` path, so it can't leave `data/`.

---

## Spam and rate limits: `code/limits.ms`

Spam is the first problem almost every public form meets. Fourfold's
defenses, from weakest to strongest:

| Defense | Default | Stops |
|---|---|---|
| Rate limits | on, for every request but GET | floods; a bot posting as fast as it can |
| `maxLinks` | per form field | link spam |
| Timing check (`minSeconds`) | off; per route | bots that post the moment they load a form |
| Proof of work (`challenge`) | off; per route | cheap bulk posting |
| [Requiring sign-in](#who-may-see-what-access-rules) | off; per route | nearly everything |

**Requiring sign-in is the strongest by far**, and is one line in
`code/access.ms`. Consider it first for anything beyond a toy.

### Default limits

Every request other than GET is counted: 60 a minute per visitor address
for visitors who aren't signed in, 300 a minute per user for those who are.
Over the limit, the visitor gets a 429 ("Too many requests") with a
`Retry-After` header, using your `pages/429.html` if you have one. Change
the defaults in `config.limits` ([Configuration](#configuration-privateconfig)).

A school or office behind one address looks like one busy visitor, so the
defaults are generous, and signed-in visitors are counted per user.
IPv6 visitors are counted by their /64 network.

### Rules per route

`code/limits.ms` is read when the app compiles, like `access.ms`. A rule
covers its path and everything below it:

```
limits = {}
limits["/guestbook/sign"] = ["5/minute", "50/day"]
limits["/api"]            = "100/minute per user"
limits["/forum/post"]     = {"rate": "10/hour", "minSeconds": 3, "challenge": true}
```

- **A rate** is `count/unit`, with `second`, `minute`, `hour` or `day`, then
  optionally `per ip` (the default), `per user` (falls back to per address
  for visitors who aren't signed in) or `per session`. `/` and ` per ` mean
  the same: `"100/minute/user"` works too.
- **Every rule that applies does**: the defaults, a folder's rule, and the
  route's own. Adding a rule only tightens. Routes under one rule share its
  count: `"/api"` is 100 a minute for all of `/api` together.
- A route's own rules count **every** request to it, GET included.
- **A map** can also set the timing check and proof of work (below).
- A rule that covers no route is a warning.

### The timing check: `minSeconds`

Every form Fourfold renders carries a signed timestamp. On a route with
`minSeconds`, a submission that comes sooner after its form was shown is
refused (429, "That was quick!"), and one with no timestamp (a bot posting
without loading the form) is refused with a 403. A bot can't forge an
earlier time; it has to wait, on every post.

It's best for forms where people type something; a one-button form would
trip it. Old forms aren't refused, since people leave tabs open, unless you
add `"maxAgeMinutes"`.

### Proof of work: `challenge`

On a route with `"challenge": true`, each form posting to it carries a small
puzzle, which the page's JavaScript solves while the visitor fills the form
in, normally before they click. Each puzzle works once. People don't notice
it; a bot posting in bulk pays for every post.

- It **needs JavaScript**: without it, the form shows "This form needs
  JavaScript to send", and a submission is refused. That's why it's opt-in.
- Fourfold adds the puzzle to forms whose `action` or `hx-post` names the
  route, and links its solver into the page. Pages that load
  `/__fourfold/htmx.js` have the solver already.
- A spammer with plenty of computing power can still pay; it reduces bulk
  spam rather than ending it.

### Limits from code: `ratelimit`

For limits only your app knows the key for:

```
if not ratelimit("comment:" + postId, "20/hour") then return status(429, "Slow down, please.")
```

`ratelimit(key, rate)` counts this request and returns 1 if it's within the
rate, else 0. A rate's `per user`, `per ip` or `per session` counts each
visitor separately.

### In dev mode

The [access report](#developing-on-your-own-machine) shows each route's
limits, timing check, puzzle and form; `/__dev/limits` shows the counters
and the latest requests counted or refused, and can reset them. Puzzles are
very easy in dev, so testing isn't slowed.

---

## Calling other servers: `http`

Your code can call other servers' APIs (weather, maps, payments, chat
webhooks) with the `http` module:

```
get.weather = function(req)
    resp = http.get("https://api.weather.gov/points/39.7,-97.1")
    if resp isa error then return status(502, "The weather service didn't answer: " + resp.message)
    return json(resp.json.properties)
end function
```

### Which hosts: `code/outbound.ms`

**Your code can reach only the hosts you list**, in `code/outbound.ms`,
read when the app compiles (like `access.ms`). With no `outbound.ms`, `http`
can reach nothing at all.

```
// code/outbound.ms
allow = ["api.weather.gov", "*.stripe.com", "http://feeds.example.org", "api.example.com:8443"]
```

- A host name allows that host only; `"*.stripe.com"` allows its subdomains
  (`api.stripe.com`), not `stripe.com` itself.
- **HTTPS only**, on port 443, unless you write `"http://host"` (which
  allows plain HTTP too) or a port (`"host:8443"`).
- `"*"` allows any public host, for things like link previews or a feed
  reader that fetch URLs visitors give you. It's allowed, with a warning, and
  stands out in the outbound report.
- Your host's admin can tighten this list (`config.outbound`), never widen it.

### What comes back

| Call | Sends |
|---|---|
| `http.get(url, headers=null)` | GET |
| `http.head(url, headers=null)`, `http.delete(url, headers=null)` | HEAD, DELETE |
| `http.post(url, body=null, headers=null)` | POST; `http.put` and `http.patch` the same |
| `http.request(method, url, body=null, headers=null)` | any of those |

Each returns a map:

| Key | Is |
|---|---|
| `status` | the status code: 200, 404, ... |
| `headers` | the response's headers, with **lowercase** names |
| `body` | the body as text, or `null` if it isn't text |
| `raw` | the body as [`RawData`](#binary-files-rawdata), when `body` is `null` (an image, say) |
| `json` | the body parsed, if the response says it's JSON; else `null` |

- **A response is a response, whatever its status**: a 404 or 500 comes back
  as a map with that `status`. Check it.
- **A request that couldn't be made returns an error value**: a host not on
  your list, a refused address, a timeout, a response that's too big.
  Check with `isa error`; `resp.message` says why.
- **A body** that's a map or list is sent as JSON; a string as text; `RawData`
  as bytes. Each gets a matching `Content-Type`, unless your headers set one.
- **Headers** are a map: `{"Accept": "application/json"}`. A few belong to
  the engine and can't be set (`Host`, `Content-Length` and the like).
- Spaces and other characters a URL can't hold are encoded for you:
  `"https://api.example.com/search?q=" + city` works with `"San Jose"`.
- **What comes back is untrusted**, like anything a visitor types. Putting
  it in a page is safe (fields are always escaped), but don't treat it as
  code, and check what you use.
- Calls wait, but the server doesn't: other visitors are served meanwhile.
  The time counts against your request's budget.

### What's always refused

Whatever your list says, the engine never connects to private or internal
addresses: `localhost` and 127.0.0.1, your network's private ranges
(10.x, 192.168.x, ...), and 169.254.169.254, where cloud servers keep their
credentials. It checks the address it actually connects to, every time, so
a name that points there (or starts pointing there later) is refused too.
If your app really needs an internal service, your admin can allow its
address (`config.outbound.allowAddresses`).

**Redirects** are followed, at most 5, and each one is checked the same way:
a redirect to a host not on your list, to a private address, or from HTTPS
to plain HTTP, fails. A 303 (and a 301 or 302 after a POST) becomes a GET,
as in a browser.

### Limits

| Limit | Default |
|---|---|
| Time, for the whole request (redirects included) | 10 seconds, and never past your request's own budget |
| Response size (after decompression) | 5 MB |
| Requests, for the whole app | 120 a minute |

Your admin can change these (`config.outbound`; see
[Configuration](#configuration-privateconfig)).

### Seeing what your code sent

Every request is written to the engine log (`private/state/logs/`): host,
status, time and size, with the query string and any secrets left out. In
dev mode, `/__dev/outbound` shows your allowed hosts, your secrets (names
only), your webhooks, and the latest requests, refused ones included, with
why.

---

## Secrets: API keys in your code

An API key goes in `private/config/secrets.ms`, with the hosts it may be
sent to:

```
// private/config/secrets.ms
secrets = {}
secrets.weatherKey = {"value": "abc123", "hosts": ["api.weather.example"]}
secrets.stripeHook = {"value": "whsec_...", "verify": true}
secrets.smtpPassword = {"value": "..."}
```

Your code sees each one as `secrets.name`: **a handle, not the key**. It
can't read the key, print it, or change it. It can only send it, in a URL or
a header, and the engine puts the real value in as the request goes out,
only to that secret's hosts:

```
resp = http.get("https://api.weather.example/today", {"Authorization": ["Bearer ", secrets.weatherKey]})
resp = http.get(["https://api.weather.example/today?key=", secrets.weatherKey])
```

- A URL or header value can be **a list of strings and secrets**, joined as
  the request goes out. That's how to write `"Bearer "` in front of a key.
- **A secret goes only to its own hosts.** Sent anywhere else (a typo, or a
  URL a visitor steered), the request isn't sent; a redirect elsewhere
  fails too. `"*.example.com"` in `hosts` covers subdomains.
- A secret in a URL goes only in its path or query (encoded for you), never
  its host.
- **It can't end up anywhere else.** `str(secrets.weatherKey)` is just a
  placeholder; putting a secret in a page, `data`, the `session`, `json()`
  or a mail is an error. So a bug can't leak a key into a page or a log.
- **Three kinds:** a secret with `hosts` can be sent to them; one with
  `"verify": true` can [check webhook signatures](#being-called-webhooks)
  but is never sent anywhere; one with neither is for the engine only (mail
  and database passwords), and your code can't use it at all.

Keep `secrets.ms` out of version control. `app.ms` can be shared;
`secrets.ms` can't.

---

## Being called: webhooks

Payment providers, GitHub and others call **into** your app: they POST to a
URL you give them when something happens. They aren't browsers, so they
can't send a [CSRF token](#forms-and-csrf-protection), and an ordinary
`post` handler refuses them. A webhook handler is for them:

```
// code/pages/pay.ms
webhook.stripe = function(req)
    sig = ""
    if req.headers.hasIndex("x-signature") then sig = req.headers["x-signature"]
    if not hmac.verify("sha256", secrets.stripeHook, req.rawBody, sig) then return status(400, "bad signature")
    event = req.json
    ...
    return "ok"
end function
```

- `webhook.stripe` in `pages/pay.ms` answers `POST /pay/stripe`, with **no
  CSRF check**. It answers POST only.
- **In place of the token, check the caller's signature.** Most providers
  sign each call with a key you share with them: put it in `secrets.ms`
  with `"verify": true`, and check with `hmac.verify`.
- `req.rawBody` is the body exactly as sent (what the signature covers), and
  `req.json` is the body parsed, if it's JSON. Only webhooks get them.
- Access rules and rate limits still apply; a webhook URL is usually public,
  since the caller isn't signed in. (A route's `minSeconds` and `challenge`
  don't apply: there's no form.)
- A `post.x` and a `webhook.x` with the same name are an error: both would
  answer the same POST.

`hmac.verify(algorithm, secret, message, signature)` returns 1 if
`signature` is the HMAC of `message` with the secret, else 0. The algorithm
is `"sha256"` (the only one so far); the signature may be hex or base64,
and GitHub's `"sha256="` prefix is fine. Some providers sign more than the
body (Stripe signs a timestamp, a dot, then the body): build the message as
their docs say. The check takes the same time whether it matches or not.

---

## Sending email: `mail.send`

```
post.signup = function(req)
    ...
    mail.send req.user.id, "Welcome!", "welcome", {"name": req.user.name}
    return redirect("/thanks")
end function
```

```markdown
<!-- code/mail/welcome.md -->
# Welcome, {{name}}!

Thanks for joining. Here's how to get started: ...
```

`mail.send(to, subject, template, fields=null, options=null)` sends one
message:

- **`to` is a user's id** (the engine finds their address), or an address
  your `code/outbound.ms` lists in `mailTo`:

  ```
  mailTo = ["owner@example.com", "*@staff.example.com"]
  ```

  That's for things like a contact form that mails the site's owner. Any
  other address is refused, so your app can't be made to mail strangers.
  (`mailTo = ["*"]` allows any address, with a warning; it's rarely what
  you want.)
- **Blind copies**: `{"bcc": "owner@example.com"}` in `options` (or a
  list of up to 10) sends each the same message, by the same rules as `to`:
  a user's id, or an address in `mailTo`. They're in no header, so the
  recipient doesn't see them, and they don't see each other:

  ```
  mail.send user.id, "Your order shipped", "shipped", order, {"bcc": "owner@example.com"}
  ```

  There's no CC, on purpose: it would show users' addresses to each other.
  To copy someone openly, send them a message of their own.
- **The body is a Markdown template** in `code/mail/` (`welcome` is
  `code/mail/welcome.md`), filled with `fields` like a
  [fragment](#fragments-and-includes), and sent both as text (the Markdown
  itself) and as HTML. Includes of Markdown fragments work.
- **The subject is plain text**, one line. Nothing a visitor typed can add a
  header or a recipient: there are no raw headers to add.
- It returns `null` once the mail server has the message, or an error value.
- The message comes from `config.mail.from`, through the mail settings in
  [Configuration](#configuration-privateconfig). In dev mode it's printed in
  the terminal instead of sent.
- The whole app may send 100 messages an hour (`config.mail.appPerHour`),
  counting each recipient, blind copies included. If any recipient is
  refused (or the message would go over the limit), nothing is sent.

Sending waits for the mail server, which can take a second or two. To send
without making the visitor wait, send from a
[background job](#background-jobs-codejobs): `enqueue "sendWelcome",
{"user": id}`.

---

## Background jobs: `code/jobs`

Some work shouldn't make a visitor wait, or doesn't belong to any request:
sending a welcome email, a nightly cleanup, a daily digest, checking a feed
every few minutes. That's a **job**: a function in a file in `code/jobs/`,
run by Fourfold in a fresh VM, like a handler, but with no visitor.

```
// code/jobs/mail.ms
job.sendWelcome = function(args)
    u = users.get(args.user)
    if u == null then return null    // the account is gone: nothing to do
    return mail.send(u.id, "Welcome!", "welcome", {"name": u.name})
end function
```

- Files in `code/jobs/` define jobs in a map called `job`, the way page files
  define `get` and `post`. (Only these files have `job`; page files don't.)
  Each job's name is its key, `sendWelcome` here, and must be unique across
  `code/jobs/`.
- **No URL reaches a job.** It runs only when your code enqueues it, when the
  schedule says, or from the dev tools' "run now".
- It gets `args`, a frozen map (the one it was enqueued with), instead of
  `req`. There's no `req`, `session` or signed-in user: a job acting for a
  user is given the user's `id`, and looks them up when it runs, so it sees
  their current roles.
- `data`, `file`, `db`, `users`, `http`, `secrets`, `mail`, `enqueue` and
  `publish` all work as they do in a handler.
- **Returning an error value means it failed**, and it's tried again later
  (below). Anything else, `null` included, means it succeeded.

### Enqueuing: `enqueue`

```
post.signup = function(req)
    ...
    enqueue "sendWelcome", {"user": id}
    enqueue "remind", {"user": id}, {"delay": 3 * 24 * 60 * 60}   // in 3 days
    return redirect("/welcome")
end function
```

`enqueue name, args=null, options=null` adds a job to the queue:

- **`args` must be data**: numbers, strings, lists and maps, as for
  `session`, at most 64 KB. A function or a secret handle is an error, at the
  call. Pass an id, and look the rest up in the job.
- **The jobs a request enqueues are kept only if it succeeds.** If the
  request ends in an error, they're dropped (in dev mode, the terminal says
  so). A failed signup sends no welcome email. The same goes for a job: what
  it enqueues is kept only if it succeeds, so jobs can enqueue follow-ups.
- **`{"delay": seconds}`** waits at least that long before running it.
- **`{"key": "digest:" + id}`** makes it unique while it's waiting: enqueuing
  the same key again does nothing. That covers "at most one digest" without
  bookkeeping of your own. (Once it has started, the key can be queued
  again.)
- A name with no job is an error at the call: `enqueue "sendWelcom"` says
  there's no job named `sendWelcom`.

The queue is kept in `private/state/`, where your code can't read or change
it, and it survives restarts and deploys.

### On a schedule: `code/schedule.ms`

```
// code/schedule.ms
schedule = {}
schedule["cleanup"]   = "daily 03:00"
schedule["digest"]    = "weekly mon 08:00"
schedule["pollFeeds"] = "every 15m"
schedule["report"]    = "0 6 1 * *"           // cron's five fields work too
```

| Written | Runs |
|---|---|
| `"every 15m"`, `"every 2 hours"`, `"every day"` | that long after the last run (at least a minute); the first run is when the schedule first appears |
| `"hourly"`, `"hourly :15"` | at that minute of every hour |
| `"daily 03:00"` | at that time each day (24-hour) |
| `"weekly mon 08:00"` | on that weekday (`mon` ... `sun`) |
| `"monthly 1 06:00"` | on that day of the month (1 to 28) |
| `"0 6 * * 1-5"` | cron: minute, hour, day of the month, month, weekday |

- Times are in the app's time zone, `config.jobs.timeZone` (a name like
  `"America/Denver"`; UTC unless you set one).
- **Daylight saving time**: a time skipped when clocks go forward (02:30 on
  the spring night, in the US) runs at the jump, 03:00. A time that happens
  twice when clocks go back runs once, the first time. `"every"` intervals are
  real time, so DST doesn't stretch or shrink them.
- **A scheduled job never overlaps itself.** If the last run is still waiting
  or running when the next is due, that one is skipped.
- **Missed runs run once.** If the server was down at 03:00, `"daily 03:00"`
  runs once when it starts, not once per missed day.
- `schedule.ms` is read when the app compiles, as data, like `access.ms`. A
  schedule naming no job, or one that isn't a schedule, is a compile error.

### When a job fails

- A job that fails (an error value, a runtime error, or running out of time)
  is tried again: after 15 seconds, then 30, a minute, two..., up to an hour
  apart. After `config.jobs.maxAttempts` (5) it's **dead**: kept with its last
  error, in the engine log, and in the jobs panel, where you can retry or
  discard it.
- **A job can run more than once.** If the server stops while one runs, it
  runs again when the server starts. So write jobs that are safe to repeat:
  `data.increment`, `data.compareAndSet`, and a `key` help.
- Each job runs on whichever version of the app is current when it starts.
  If a deploy removes a job that still has entries in the queue, the terminal
  (or engine log) warns, and those entries go dead when they come up.

### Limits

- A job's own budget: 100 million instructions and 60 seconds
  (`config.jobs.instructions`, `config.jobs.seconds`), more than a request's.
- At most 2 jobs run at once (`config.jobs.workers`), so jobs can't crowd out
  visitors' requests.
- At most 100 `enqueue`s per request or job.
- On shared hosting (CGI or FastCGI), jobs run only when cron runs
  `fourfold run-jobs`, usually once a minute: a queued job waits until then,
  and a schedule finer than a minute can't be kept. See the admin guide's
  [Shared hosting](../admin/admin_guide.md#shared-hosting-cgi-and-fastcgi).

### In dev mode

The **jobs panel**, at `/__dev/jobs` (also in the corner menu), shows the
schedule with each job's next and last run, the queue (waiting, running and
dead jobs, with their arguments, attempts and last error), the latest runs,
a "run now" button for every job, and a toggle that pauses the schedule
(queued jobs still run). Each run is printed in the terminal. Scheduled jobs
run in dev mode just as on a server.

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

## Live updates: `publish`

A page can get new content the moment something happens, without reloading
or asking: a new chat message, a finished order, a story a job just found.
The page subscribes to a **channel**, and your code **publishes** to it.

```html
<!-- in the page: HTMX's SSE extension comes with /__fourfold/htmx.js -->
<ul hx-ext="sse" sse-connect="/live/guestbook" sse-swap="message" hx-swap="beforeend">
  {{entries}}
</ul>
```

```
post.sign = function(req)
    entry = {"who": req.form.who, "msg": req.form.msg}
    data.append "guestbook", entry
    publish "guestbook", frag("entry", entry)    // to every open page
    if req.htmx then return null
    return redirect("/guestbook")
end function
```

- A page subscribes with `sse-connect="/live/<channel>"`; HTMX keeps the
  connection open (and reconnects if it drops). `sse-swap="message"` puts
  each message into the element, as `hx-swap` says.
- `publish channel, content, event="message"` sends `content`, a
  `frag(...)`, `markdown(...)`, or a string (sent as text, escaped), to every
  page subscribed to `/live/<channel>`. Use another event name to send to a
  different element: `publish "orders", frag("count", n), "count"` goes to the
  element with `sse-swap="count"`.
- Like `enqueue`, a request's (or job's) messages are sent only if it
  succeeds.
- **A channel needs an access rule.** `/live/<name>` is a path like any
  other, and `code/access.ms` decides who may subscribe. A channel no rule
  covers isn't served (404), so you choose which exist:

  ```
  rules["/live/news"] = "public"       // anyone
  rules["/live/members"] = "login"     // signed-in visitors only
  rules["/live/staff"] = "admin"       // and so on, as for pages
  ```

  A rule covers everything below it, so `rules["/live"] = "public"` makes
  every channel public. A visitor who may not subscribe gets a 401 (not
  signed in) or 403, and no messages.
- Channel names are letters, digits, `-` and `_`, in parts joined by `/`
  (`"rooms/lobby"`). `code/pages/live/` can't exist: `/live/` is for
  channels.
- A message can't hold a `<script>` (it wouldn't run), or a form rendered
  for the visitor who published it (its CSRF token would go to everyone).
  Publish HTML without forms; a subscriber can load a form with `hx-get`.
- At most 64 KB per message, and 100 messages per request or job. Every
  subscriber to a channel gets the same message: per-user channels aren't
  here yet.
- Live updates need `fourfold dev` or `fourfold serve`, which keep the
  connections open. On shared hosting (CGI or FastCGI; see the admin
  guide's [Shared hosting](../admin/admin_guide.md#shared-hosting-cgi-and-fastcgi)),
  `/live/` isn't served and `publish` does nothing. Have the page poll
  instead, which works everywhere:

  ```html
  <ul hx-get="/guestbook/latest" hx-trigger="every 5s">{{entries}}</ul>
  ```

In dev mode, the jobs panel lists the open channels and how many pages
subscribe to each, and the access report lists who may subscribe.

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
| Request body | 1 MB, plus a form's declared uploads | 413 |
| Fields in a form; parts in an upload form | 1,000; 100 | 413 |
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
- `data/`, sessions, users and `dropbox/` are untouched by a deploy.
- Changes to `private/config/` don't need a deploy: they're read again on
  their own (see [Configuration](#configuration-privateconfig)).

**A deploy with new [migrations](#changing-the-datas-shape-migrations)**
compiles, but doesn't go live on its own: the old version keeps serving, and
the engine log says how many migrations are waiting. When you're ready,
apply them:

```sh
fourfold migrate path/to/myapp
```

It asks the running server to hold requests, back up, migrate and switch to
the new version, and prints the outcome. (If no server is running, it does
the work itself.) A fresh install has every migration pending, so
`fourfold serve` won't start until you've run `fourfold migrate`, or you
start it with `fourfold serve --migrate`.

Since anything in `code/` with an error stops the whole deploy, it's best to
check your changes locally first.

**On shared hosting** (CGI or FastCGI), deploying works the same way, with
two differences: new code also goes live on its own once uploads have
stopped for a couple of seconds (touching `code/.deploy` still makes it
immediate), and new migrations are applied by the first request on the new
version, with a backup first, rather than by `fourfold migrate`. If one
fails, nothing changes and the old version keeps serving. See the admin
guide's [Shared hosting](../admin/admin_guide.md#shared-hosting-cgi-and-fastcgi).

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
- **Sign in as a test user**: the small menu in the corner of every page (or
  the sign-in page) has "sign in as...", which offers an admin, a member with
  no roles, and a user for each other role your access rules name. Each has
  its own session, so switching doesn't mix them up.
- **Sign-in emails are printed** in the terminal instead of sent (unless
  `config.mail` sets up a real transport), so you can follow the link.
- **The access report**, at `/__dev/access` (also in the corner menu): every
  URL, who may use it, and why; and its rate limits, spam checks and form.
- **Rate limits**, at `/__dev/limits` (also in the corner menu): the counters,
  the latest requests counted or refused, and a button to reset them.
- **The outbound report**, at `/__dev/outbound` (also in the corner menu):
  the hosts your code may reach, the addresses it may mail, your secrets
  (names and hosts, never values), your webhooks, and the latest outbound
  requests, refused ones included, with why.
- **Mail from `mail.send`** is printed in the terminal too, like sign-in
  emails.
- **The jobs panel**, at `/__dev/jobs` (also in the corner menu): the
  schedule, the queue, recent runs, dead jobs with retry and discard, "run
  now" for any job, a toggle that pauses the schedule, and the open
  [live channels](#live-updates-publish).
- **Migrations** are applied as soon as you save one (with a backup first),
  and `code/seed.ms` fills a fresh database with sample data. `/__dev/migrations`
  shows which are applied, pending, failed or changed, with two buttons:
  **redo the last migration** (go back to the backup from before it, and
  apply what's there now: for editing a migration until it's right) and
  **reset dev data** (empty `data/` and the database, after a backup, then
  run every migration and the seed). Both work with SQLite only.
- **Proof-of-work puzzles** are very easy, so testing isn't slowed.

Dev mode listens only on your own machine (`localhost`), over plain HTTP.
It uses the app's real `data/` folder, so work on a copy if the data matters.

---

## Quick reference

### Files

| Path | Is |
|---|---|
| `code/pages/x.html` | the page at `/x` |
| `code/pages/x.ms` | its code: `render`, `get.*`, `post.*`, `webhook.*` |
| `code/pages/x.md` | a Markdown page at `/x`, and as Markdown at `/x.md` |
| `code/pages/dir/index.html` | the page at `/dir/` |
| `code/pages/404.html`, `500.html`, `429.html`, `503.html` | the error pages (503: while migrations run too long) |
| `code/pages/markdown.css` | styles for rendered Markdown |
| `code/frags/x.html`, `x.md` | the fragment `x` |
| `code/frags/layout/markdown.html` | the page around Markdown pages |
| `code/lib/x.ms` | the module for `import "x"` |
| `code/access.ms` | access rules: `rules["/path"] = "login"` |
| `code/limits.ms` | rate limits and spam checks: `limits["/path"] = "5/minute"` |
| `code/outbound.ms` | hosts `http` may reach, `allow = [...]`, and addresses `mail.send` may write to, `mailTo = [...]` |
| `code/mail/x.md` | the mail template `x` |
| `code/jobs/x.ms` | jobs: `job.name = function(args) ... end function` |
| `code/schedule.ms` | when jobs run: `schedule["name"] = "daily 03:00"` |
| `code/queries/x.sql` | the named query `x`; `x.sqlite.sql`, `x.pg.sql` for one database |
| `code/migrations/001_x.sql` | a migration: SQL, a folder with `migrate.ms`, or a `.ms` file |
| `code/seed.ms` | sample data for a fresh dev database |
| `code/frags/auth/layout.html` | the page around the sign-in pages |
| `code/.deploy` | touch it to deploy |
| `private/config/app.ms` | the configuration: `config = {}` |
| `private/config/secrets.ms` | passwords and keys: `secrets = {}` |

### Template syntax

| Syntax | Means |
|---|---|
| `{{name}}` | a field |
| `{{user.name}}` | a field inside a map |
| `{{> header}}` | include `frags/header` |
| `{{key}}` in a `.sql` file | a value for the query: `{{title}}` from a map, `{{0}}` from a list |
| `{{flash}}` | `session.flash`, if any; showing it removes it |
| `\{{` | a literal `{{` |
| `[[` | reserved in `.md` files in `code/`; write `\[[` |

### Functions

| Call | Returns |
|---|---|
| `frag(name, value)` | rendered HTML; a list of maps renders it once each |
| `markdown(text, profile="limited")` | rendered Markdown, Limited or `"full"` (`text` can be a list of lines) |
| `unsafeHTML(text)` | `text`, as HTML, unchecked |
| `redirect(url)` | a redirect response |
| `json(value)` | a JSON response |
| `status(code, body=null)` | a response with any status |
| `formState(name)` | the map for `form.name`'s fragment: values and `errors` |
| `reshow(fragName, extraErrors=null)` | the declared form again, with its errors |
| `ratelimit(key, rate)` | 1 if within the rate (and counted), else 0 |
| `file.send(path, downloadName=null)` | a response sending a file from `data/` |
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
| `users.get(id)`, `users.find(email)` | the user's map, or null |
| `users.grant(id, role)`, `users.revoke(id, role)` | null |
| `db.query(name, values=null)` | the rows, a list of maps |
| `http.get(url, headers=null)` | `{status, headers, body, raw, json}`, or an error; also `head`, `delete` |
| `http.post(url, body=null, headers=null)` | the same; also `put`, `patch`, and `http.request(method, url, body, headers)` |
| `hmac.verify(algorithm, secret, message, signature)` | 1 if the signature matches, else 0 |
| `mail.send(to, subject, template, fields=null, options=null)` | null, once sent; `to` (and each of `options.bcc`) is a user's id or an address in `mailTo` |
| `enqueue name, args=null, options=null` | null; queues `job.name`, kept if the request (or job) succeeds; options `delay`, `key` |
| `publish channel, content, event="message"` | null; sends HTML to the pages on `/live/channel`, if the request (or job) succeeds |

### Globals

| Name | Is |
|---|---|
| `req` | the request (frozen); `req.user` is the signed-in visitor, or null |
| `access` | set in a page's `.ms` file: who may use its page and handlers |
| `session` | this visitor's map, saved if changed; `session.flash` is removed once a page shows it |
| `get`, `post` | maps of handlers |
| `webhook` | a map of handlers for POSTs from other servers, without CSRF checks |
| `job` | in `code/jobs/`: a map of jobs |
| `secrets` | handles for the secrets in `secrets.ms` that your code may use |
| `form` | a map of form declarations: `form.sign` for `post.sign` |
| `migrate` | in a migration: the function that makes the change |
| `render` | the page's render function, if it has one |

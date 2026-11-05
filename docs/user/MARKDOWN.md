# FourFold Flavored Markdown

Markdown is a first-class format in Fourfold. A page can be a `.md` file, a
fragment can be a `.md` file, and code can render Markdown with
`markdown(text)`. Every Markdown page can also be fetched *as Markdown*, so
a site written in Markdown can be read by a Markdown-only client, such as a
Markdown browser in Mini Micro, a command-line tool or an AI agent.

The Markdown is **GitHub-flavored Markdown (GFM), made safe**. This document
describes it completely: what it allows, the three levels it comes in, how
fields work in Markdown files, and what a Markdown client is sent. The
[user guide](user_guide.md#markdown) shows how it fits into an app.

*Status: as of Milestone 6.1.*

---

## Contents

1. [The three levels](#the-three-levels)
2. [What every level has](#what-every-level-has)
3. [Limited Markdown](#limited-markdown)
4. [Full Markdown](#full-markdown)
5. [Dynamic Markdown](#dynamic-markdown)
6. [Markdown as Markdown: /x.md](#markdown-as-markdown-xmd)
7. [Quick reference](#quick-reference)
8. [Looking ahead](#looking-ahead)

---

## The three levels

| Level | Where it's used | What it allows |
|---|---|---|
| **Limited Markdown** | `markdown(text)` from code: the default, for anything a visitor wrote, such as comments and posts | the basics, with headings shown as paragraphs, images shown as links, and links marked `rel="nofollow ugc"` |
| **Full Markdown** | `markdown(text, "full")` from code, for text you trust | everything below, with heading ids |
| **Dynamic Markdown** | `.md` files in `code/` (pages and fragments) | Full Markdown, plus fields (`{{name}}`) and includes (`{{> intro}}`), so code can change what the visitor sees |

Text given to `markdown()` is never dynamic: `{{` in it is just text, so a
visitor can't write a field. The same goes for Markdown kept in `data/`.
Only your own files in `code/` have fields.

In code, the levels are named by `markdown()`'s second argument:
`markdown(text)` and `markdown(text, "limited")` are Limited Markdown, and
`markdown(text, "full")` is Full Markdown. (Before Milestone 6.1, `"limited"`
was called `"comment"`.)

---

## What every level has

All of GitHub-flavored Markdown, except raw HTML and footnotes:

| Feature | Written as |
|---|---|
| Paragraphs and line breaks | a blank line between paragraphs; two spaces or `\` at a line's end for a line break |
| Emphasis | `*italic*`, `**bold**`, `~~struck~~` |
| Code | `` `code` ``, and code blocks fenced with ```` ``` ```` or `~~~` (with a language: ```` ```js ````), or indented four spaces |
| Lists | `- item`, `* item`, `+ item`, `1. item`, `1) item`, nested by indenting |
| Task lists | `- [ ] to do`, `- [x] done` (the checkboxes can't be clicked) |
| Block quotes | `> quoted` |
| Tables | `\| a \| b \|`, then `\|---\|:-:\|` (alignment), then rows |
| Links | `[text](url "title")`, `[text][ref]` with `[ref]: url`, `<https://...>` |
| Bare links | `https://example.com`, `www.example.com` and `me@example.com` become links |
| Images | `![alt text](url "title")` |
| Headings | `# One` to `###### Six`, or a line underlined with `===` or `---` |
| Thematic breaks | `***`, `---` or `___` on a line of its own |
| Character references | `&amp;`, `&copy;`, `&#169;` |
| Escapes | `\` before any punctuation shows it as is: `\*not italic\*` |

And everywhere, these rules:

- **No raw HTML.** HTML tags in Markdown are shown as text, even in your own
  files: `<b>x</b>` shows as `<b>x</b>`, not as bold. That keeps Markdown from
  ever running script. For HTML, write an HTML fragment.
- **Links and images follow the URL rule**: `http:`, `https:`, `mailto:` or a
  relative URL. Anything else (`javascript:`, `data:`, ...) is shown as the
  link's text, with no link.
- **Code blocks** become `<pre><code class="language-js">`, so a syntax
  highlighter in the browser can color them.
- **No footnotes yet**, and no math or wiki links.
- **Rendered Markdown is wrapped** in `<div class="markdown">` (or
  `<div class="markdown limited">` for Limited Markdown), and pages that show
  it link a stylesheet (your `pages/markdown.css`, or a built-in one). See the
  [user guide](user_guide.md#styling).

---

## Limited Markdown

Text from visitors is rendered as Limited Markdown unless you ask for more.
It differs from Full Markdown in three ways, each for a reason:

| Written | Becomes | Why |
|---|---|---|
| `# Heading` | an ordinary paragraph | A visitor's `# Hello` shouldn't be the biggest text on your page. |
| `![alt](url)` | a link to the image, with the alt text (or the URL) as its text | A visitor's image could be a tracking pixel, or someone else's bandwidth. |
| `[text](url)` | a link marked `rel="nofollow ugc"` | It tells search engines the link is user content, which takes away the reason to spam it. |

Limited Markdown also gives nothing an `id`. A visitor's text that made
`id="entries"` could take over your page's own `#entries`, which your HTMX or
CSS may rely on.

---

## Full Markdown

Full Markdown is everything in [What every level has](#what-every-level-has),
plus:

- **Heading ids** in GitHub's style: `## Hello, World!` gets
  `id="hello-world"`, so a `#hello-world` link goes to it. A repeated heading
  gets `-1`, `-2`, ... added.
- **Images** are shown.
- **The page title.** For a Markdown page, the first `#` heading is the
  page's title: what the layout's `{{title}}` shows.

---

## Dynamic Markdown

Every `.md` file in `code/` is a template, like an HTML file: a page in
`code/pages/` or a fragment in `code/frags/`. It can have fields, filled by
the page's code, and include other Markdown fragments.

```
# Hello, {{user.name}}

You have **{{count}}** new messages.

{{> footer}}
```

### Fields

Fields work as in [HTML templates](user_guide.md#templates):

- `{{name}}` is a field, `{{user.name}}` is a field inside a map, and
  `{{> intro}}` includes `frags/intro.md`.
- A page's fields are filled by `render` in the page's `.ms` file:
  `about.md` pairs with `about.ms`, as `about.html` does.
- A fragment's fields are filled by `frag("name", value)`, and
  `frag("row", list)` renders it once for each item.
- A page with no code renders its fields empty. A page with a `{{flash}}`
  field shows [`session.flash`](user_guide.md#one-time-messages-sessionflash).
- A value can be a string or a number; null is empty; a map or list is an
  error.

**Parse first, fill second.** Fourfold reads the Markdown once, when the app
is compiled, with each field as a placeholder, and notes where each one is.
A value is put in afterwards, so it can never change the Markdown around it:
a value of `**hi**` or `# title` or a line break shows as exactly that text.

### Where a field can go, and how its value is written

The value is escaped for where the field is, in both the HTML a browser gets
and the Markdown a Markdown client gets:

| Where the field is | Example | In the HTML | In the Markdown |
|---|---|---|---|
| Text: a paragraph, heading, list item, table cell, emphasis, link text | `Hi, *{{name}}*` | escaped as HTML | every punctuation character escaped with `\`; line breaks and tabs as spaces; spaces at the start or end as `&#32;` |
| An image's alt text, or a link's title | `![{{alt}}](cat.png)`, `[a](/x "{{tip}}")` | escaped for the attribute | as text, above |
| The start of a link or image URL | `[profile]({{url}})` | the URL rule: if it isn't `http`, `https`, `mailto` or relative, it's replaced with `about:invalid#fourfold` (and dev mode warns) | the same, with spaces, `(`, `)`, `<` and `>` percent-encoded |
| Later in a URL | `[next](list.md?page={{n}})` | percent-encoded | percent-encoded |
| Alone on a line, as a paragraph of its own | `{{entries}}` | a string is a paragraph; a rendered value is its own blocks (see below) | the same |

So a value can't add a link, an image, a heading, raw HTML, a list or a table,
in either output.

### Rendered values and includes are blocks

An include (`{{> name}}`), or a field holding something rendered (from `frag`
or `markdown`), stands for whole blocks, not text in a paragraph. It must be
**a paragraph of its own**: alone on its line, at the top level (not in a
list, quote or table), with a blank line before and after it:

```
# Guestbook

{{entries}}

{{> footer}}
```

- `{{> intro}}` anywhere else is a compile error. A rendered value in a field
  that isn't a paragraph of its own is an error when the page is rendered.
- A field alone on its line can still hold a string: it's shown as a
  paragraph.
- **A Markdown file takes only Markdown**: it includes only `.md` fragments,
  and its fields take only `frag()` of an `.md` fragment, or `markdown()`. An
  HTML fragment or `unsafeHTML` there is an error (at compile time for an
  include, at render time for a value), whether the page is being sent as
  HTML or as Markdown, so the two never disagree about what works.
- An HTML page *can* include an `.md` fragment, or show `markdown()`.

### Where fields can't go

Each of these is a compile error, with a message saying what to do instead:

| Where | Example | Instead |
|---|---|---|
| A link reference definition, or a link's label | `[x]: {{url}}`, `[text][{{ref}}]` | put the URL in the link: `[text]({{url}})` |
| Inside `[brackets]` that aren't a link | `See [{{name}}].` | escape them: `\[{{name}}\]` |
| An autolink | `<https://e.com/{{x}}>` | `[text](https://e.com/{{x}})` |
| Part of a bare web or email address | `https://e.com/{{path}}`, `www.{{host}}.com`, `{{user}}@example.com` | write a link, or escape the `@`: `{{user}}\@example.com` |
| A code block's language | ```` ```{{lang}} ```` | |
| Right after `<` | `<{{tag}}>` | |
| Inside raw HTML | `<span title="{{x}}">` | Fourfold shows raw HTML as text, but a Markdown client might render it, with the value inside |
| A URL that isn't allowed | `[a](javascript:{{x}})` | |

Two rules keep the Markdown output meaning the same as the HTML: brackets
around a field could turn the value into a link label when a Markdown client
reads it, and text next to a field could make a bare address that a client
turns into a link.

### Code, escapes, and `[[`

- **`{{` in code is just text.** In a code span or a code block, `{{name}}`
  is shown as written, so documentation can show template syntax:
  `` `{{name}}` ``.
- **`\{{` is a literal `{{`** anywhere else.
- **`[[` is reserved.** In a Markdown file in `code/`, `[[` outside code is a
  compile error, kept for a future use (form inputs; see
  [Looking ahead](#looking-ahead)). Write `\[[` for a literal `[[`, or put it
  in code. (`[[` is ordinary text in Limited and Full Markdown.)
- **Fourfold keeps the Unicode noncharacters U+FDD0–U+FDEF** for its own use,
  so a Markdown file in `code/` that contains one is a compile error. (Text
  doesn't use them.)

### Titles and heading ids

A heading can have fields. Its `id` comes from its text with the fields left
out (`## Hello, {{name}}` gets `id="hello-"`), so links to it don't change
with the value. The page's title, from its first `#` heading, includes the
values.

---

## Markdown as Markdown: /x.md

Every Markdown page is also served as Markdown, at its name with `.md`:

| File | HTML | Markdown |
|---|---|---|
| `pages/about.md` | `/about` | `/about.md` |
| `pages/docs/setup.md` | `/docs/setup` | `/docs/setup.md` |
| `pages/docs/index.md` | `/docs/` | `/docs/index.md` |
| `pages/index.md` | `/` | `/index.md` |

- **The same page.** `/about.md` is the same route as `/about`: the same
  access rules, and the same code, whose `render` fills the same fields.
  `req.format` is `"html"` or `"markdown"`, for the rare handler that cares.
- **Filled, not raw.** The response is the page's Markdown with its fields
  filled and its includes in place. A page with no fields is sent exactly as
  you wrote it.
- **No layout.** It's the page alone; `frags/layout/markdown.html` wraps only
  the HTML. The HTML page has
  `<link rel="alternate" type="text/markdown" href="/about.md">` in its head,
  so clients can find the Markdown.
- **Links are kept as written.** `[setup](setup.md)` from `/docs/about.md`
  leads to `/docs/setup.md`, which is the Markdown of that page. (In the
  HTML, it's rewritten to `/docs/setup`.)
- **Visitors' text keeps its rules.** A `markdown(text)` value in the page is
  written out as Markdown under its level's rules: Limited Markdown's headings
  become paragraphs and its images links, links whose URLs aren't allowed
  become their text, and HTML stays escaped text. Reference links become
  ordinary links. So a Markdown client gets nothing a browser wouldn't.
- **Your own raw HTML** (`<b>` in your `.md` file) is sent as written.
  Fourfold's renderer shows it as text, and so will Mini Micro's; a generic
  GFM client might render it.
- **Only Markdown pages** have a Markdown URL: `/contact.md` for an HTML page
  is not found.
- **The type** is `text/markdown; charset=utf-8; variant=GFM`.
- **Reading only.** A Markdown client can follow links, including ones with
  query strings (`list.md?page=2`), but Markdown has no forms, so it can't
  post, and it can't sign in.

---

## Quick reference

| Syntax | In | Means |
|---|---|---|
| `{{name}}`, `{{a.b}}` | Dynamic | a field |
| `{{> name}}` | Dynamic | include `frags/name.md`, as a paragraph of its own |
| `\{{` | Dynamic | a literal `{{` |
| `[[` | Dynamic | reserved: write `\[[` |
| `` `{{x}}` `` | Dynamic | text, as written |
| `markdown(text)` | code | Limited Markdown, rendered |
| `markdown(text, "full")` | code | Full Markdown, rendered |
| `frag("x", value)` | code | `frags/x.md` (or `.html`), filled |
| `req.format` | code | `"html"`, or `"markdown"` for `/x.md` |

---

## Looking ahead

Markdown can't post, because it has no forms. A later milestone may let
Dynamic Markdown declare form inputs inline, with syntax like
`[[age:type=int,width=5]]`, building on declared forms (Milestone 7), so a
Markdown page, and a Markdown client, could post as well as read. That's why
`[[` is reserved now: pages written today won't change meaning when it comes.

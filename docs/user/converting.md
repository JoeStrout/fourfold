# Converting an existing website to Fourfold

Brief notes on what comes up when you move an existing static site into
Fourfold, and how to handle each thing. Fourfold reads every `.html` file at
compile time, so most of these show up as compile errors, each naming the
file and line. The goal of each step is a site that works the same, then one
that uses Fourfold's features: includes instead of copy-and-paste, fields
instead of hard-coded values, and code instead of client-side workarounds.

*(Started during M4, from converting the miniscript.org site. Some features
mentioned here, like `data` and forms, arrive in later milestones.)*

## Where files go

- **Whole pages** go in `code/pages/`, at the path you want them served from.
  `pages/about.html` is served at `/about`, and `pages/docs/index.html` at
  `/docs/`.
- **Images, CSS, JavaScript and other static files** stay next to the pages
  that use them, in `code/pages/`.
- **Pieces of pages** go in `code/frags/`. These are files that aren't whole
  documents: a header, a sidebar, the content of one tab, help text loaded by
  script. Fourfold refuses them in `pages/` ("a page must be a complete
  document"), since anything in `pages/` gets a URL.
- **PHP, CGI scripts, and `.htaccess`** don't run. Logic moves to a `.ms` file
  next to the page (see [Fields instead of hard-coded values](#fields-instead-of-hard-coded-values)).

## URLs

- **Pages are served without `.html`.** `pages/earn.html` is at `/earn`. A
  request for `/earn.html` redirects there (and `/docs/index.html` to
  `/docs/`), so old links and bookmarks keep working. Still, change your own
  links like `href="earn.html"` to `href="earn"`, and `index.html` links to
  the folder (`href="MiniMicro/"`), to save the extra round trip; the compile
  warnings list them.
- **A folder's index page ends in `/`**: `/docs/`. A request for `/docs`
  redirects there, so relative links inside the page work either way.
- **Broken links are warnings at compile time.** Every literal local URL in a
  page (`href`, `src`, `action`, `hx-get`, …) is checked against what the app
  serves. The warning names the file and line, and for `x.html` links, says
  what to write instead. Converting a site, this list is a good to-do list.

## File types

Static files are served only if their type is on Fourfold's list: web pages'
usual images, fonts, styles, scripts, audio, video, PDFs, `.zip`, `.txt`,
`.json` and a few more. Anything else (`.gz`, `.dmg`, an app's own formats
like `.minidisk`) gets a compile warning and isn't served, and links to it
say so. For now, zip such files; a setting to add types comes with
configuration (M6).

`.ms` files in `pages/` are a page's code, never served. To offer MiniScript
source for download, give it another extension (`.txt`), or zip it.

## Shared pieces: includes

If the same header, footer or nav is pasted into every page, move one copy to
`frags/header.html` and write `{{> header}}` where it went. Includes are pasted
in at compile time, so they cost nothing per request and need no code.

If the site fetched pieces with JavaScript and set them as `innerHTML` (tabs,
help panes), include them instead: `<div id="About">{{> MiniMicro/_about}}</div>`.
The page then works without a round trip per piece, and without script.

## Inline event handlers

`onclick="..."`, `onchange="..."` and the like are compile errors. The
Content Security Policy that Fourfold sends forbids inline JavaScript, so
they wouldn't run anyway. Mark the element with a `data-` attribute instead,
and attach the handler from a script file:

```html
<a href="#about" class="tablinks" data-tab="About">About</a>
```

```js
document.querySelectorAll(".tablinks").forEach(link => {
    link.addEventListener("click", e => openTab(e, link.dataset.tab));
});
```

When many links on a page share one handler, as in help text full of
`onclick="help_select('tut-lists'); return false;"`, one listener on a
container is simpler:

```js
document.addEventListener("click", e => {
    const link = e.target.closest("[data-help]");
    if (!link) return;
    e.preventDefault();
    help_select(link.dataset.help);
});
```

Some handlers only exist to fetch something and put it in the page. Those
are often simpler as HTMX attributes (`hx-get`, `hx-target`) with no
JavaScript at all.

The same applies to `javascript:` links (`href="javascript:go()"`): make them
buttons or links with a `data-` attribute and a listener.

## Scripts

- **Inline `<script>` elements work.** Fourfold adds the CSP nonce to every
  `<script>` in your templates, both inline ones and `src=` ones (including
  ones from a CDN).
- **A script can't contain the text `<!--` or `<script`** (for example
  `document.write("<script ...>")`). Browsers parse the rest of the page
  differently after them. Split the string (`"<scr" + "ipt"`), or better,
  create the element with `document.createElement("script")`.
- **To pass values from code to a script, use a field** where a value goes:
  `const config = {{config}};`. Or put the data in its own element and read
  it from a script file:

  ```html
  <script type="application/json" id="stats">{{stats}}</script>
  ```

  Either way the value is written as JSON, safely. A field inside a JavaScript
  string (`"{{name}}"`) is a compile error; drop the quotes, and the field
  becomes a JSON string.

## Literal `{{`

`{{` starts a field. Text that really contains `{{` needs `\{{`. This comes up
in code samples (Mustache, Handlebars, Vue, MediaWiki templates, Jinja) and in
pages that use a client-side template library. In a `.md` page, `{{` is
always literal text, so documentation with many examples may be easier as
Markdown.

## Fields instead of hard-coded values

Values that change, or that repeat across pages, can become fields. Examples
are a version number, a release date, or the base URL of download links. Put
`{{release.version}}` or `href="{{filesUrl}}/app.zip"` in the template, and
return the values from `render` in the page's `.ms` file:

```
render = function(req)
    return {"release": {"version": "1.2.6", "date": "Nov 07, 2025"},
            "filesUrl": "https://example.com/files"}
end function
```

Where a field can go:

- **Anywhere in text, and in quoted attribute values.** Unquoted values
  (`title={{x}}`) are refused.
- **At the start of a URL, the value must be http, https, mailto, or
  relative.** Anything else (`javascript:...`) is replaced with an inert URL.
- **A list of things** (entries, products, links) is one fragment rendered per
  item: `{{items}}` filled with `frag("item", list)`.
- **Not in `style` attributes, `<style>` elements, comments, or inside
  `<svg>`.** Choose a class in code instead (`class="{{state}}"`).

## Inline SVG

Inline `<svg>` works, but it can't contain fields, and HTML elements inside it
(`<p>`, `<div>`, `<span>`, …), `<style>` and `<script>` are refused. Browsers
end the SVG early at those, so the page wouldn't mean what it seems to. If an
icon needs a field, move the SVG to a file and use `<img src="{{icon}}">`.

## Markdown

- **Client-side Markdown** (a library like showdown converting text with
  `innerHTML`) can usually move to the server. Use `.md` pages for whole pages,
  `.md` fragments for pieces, or `markdown(text)` from code. That way raw HTML
  in the text is shown as text, and `javascript:` links are dropped.
- **Links between `.md` files** (`[setup](setup.md)`) are rewritten to their
  URLs, and a link to a page that doesn't exist is a warning.

## Things that are fine as they are

- `style="..."` attributes (inline styles are allowed by the CSP).
- `<meta http-equiv>` tags without fields.
- External scripts and stylesheets.

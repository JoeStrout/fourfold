# About this guestbook

It's the guestbook from Fourfold's design notes, and it shows M4's pieces:

- **Templates with fields.** `index.html` has fields like `{{count}}`, filled by
  `render` in `index.ms`. Every value is escaped for where it goes.
- **Fragments.** Each entry is `frags/entry.html`, rendered once per item with
  `frag("entry", list)`, and again on its own when someone signs.
- **Includes.** The header is `frags/header.html`, pasted into each page with
  `{{> header}}`.
- **Markdown.** Messages go through `markdown()`, as Limited Markdown (the
  default): no images, no headings, and `javascript:` links come out as plain
  text. This page is Markdown too, in `frags/layout/markdown.html`, and is
  also served as Markdown, at `/about.md`.
- **HTMX.** The form posts with `hx-post`, and the new entry is swapped in
  without a reload. Without JavaScript, the same handler redirects back.

[Back to the guestbook](index.md)

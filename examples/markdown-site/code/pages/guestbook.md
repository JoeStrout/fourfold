# Guestbook

{{count}} entries so far. Each is `frags/entry.md`, rendered once per entry
by `frag("entry", list)` in `guestbook.ms`.

{{entries}}

Messages go through `markdown()`, as Limited Markdown: no headings, no images,
and `javascript:` links come out as plain text, in the HTML page and in its
Markdown alike.

[Home](index.md)

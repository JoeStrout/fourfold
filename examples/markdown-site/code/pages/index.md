# A Markdown site

Every page here is a `.md` file in `code/pages/`, with no HTML and no setup.
Fourfold wraps each one in a built-in page, and serves it as Markdown too:
`/guide/start` is HTML, and `/guide/start.md` is the same page as Markdown.

- [Getting started](guide/start.md)
- [Formatting](guide/formatting.md)
- [The guide's own index](guide/index.md)
- [A visitor counter](visits.md): a Markdown page with a field and code
- [A guestbook](guestbook.md): a Markdown fragment per entry

Links between `.md` files are written as file names, so the folder reads
correctly on GitHub or in an editor too; Fourfold rewrites them to URLs in the
HTML, and a Markdown client follows them as they are.

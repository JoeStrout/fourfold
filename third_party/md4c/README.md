# md4c

Markdown parser (CommonMark, with GitHub extensions). Version 0.5.2, from
https://github.com/mity/md4c (tag `release-0.5.2`): `src/md4c.c`,
`src/md4c.h`, and the HTML entity table `src/entity.c`, `src/entity.h`.
MIT license.

Fourfold uses only the parser, with `MD_FLAG_NOHTML`; the renderer is its
own (`src/markdown/`), so that heading ids, the two profiles, the URL rule
and link rewriting live in one place. `md4c-html.c` isn't vendored.

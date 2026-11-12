# htmx

The htmx library, version 2.0.4, from https://unpkg.com/htmx.org@2.0.4
(`dist/htmx.min.js`). Zero-Clause BSD license.

The engine serves it at `/__fourfold/htmx.js`, followed by its own
configuration block (no `eval`, no script tags in swapped content, no inline
indicator styles; see `src/engine/pipeline/assets.cpp`).

`sse.js` is htmx's Server Sent Events extension, version 2.2.2, from
https://unpkg.com/htmx-ext-sse@2.2.2/sse.js (Zero-Clause BSD license, in
`LICENSE-sse`). It's served as part of `/__fourfold/htmx.js` (M9.1), so
`hx-ext="sse"` works on any page that loads htmx.

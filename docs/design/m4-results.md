# Fourfold: M4 Results — Safe Templates and Markdown

*Status: 2026-10-25, at the end of M4. Records what M4 built and decided about risk R6 ([milestones.md](milestones.md), Risk register): the template rules as implemented, the Markdown renderer, the CSP nonce, HTMX, and the fuzz and timing results. Like M2 and M3, M4 made no MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| **R6**: is the contextual template parser harder in practice than on paper? | **Retired.** The parser (`src/template/parse.cpp`, about 1,100 lines, plus a 270-line JavaScript lexer) follows the HTML spec's tokenizer states. Where the spec gets subtle, it refuses rather than guesses, which kept it small. It's linear in its input, and survives fuzzing. |
| **R6**: are the rules too strict for real templates? | **Mostly retired.** The 68 HTML files of miniscript.org compile after expected conversions, with no rule found too strict (see [A real site](#a-real-site)). That site has no fields, so the rules about where fields go are tested only by the one page converted to use them. |
| Markdown | md4c's parser, with raw HTML off, and a renderer of our own for the two profiles, heading ids, the URL rule and link rewriting. **No footnotes yet** (md4c has none). |
| CSP | A fresh 128-bit nonce per response, on every `<script>` the engine renders. `script-src 'self' 'nonce-…'`; no inline script without it runs. |
| HTMX | htmx 2.0.4, served at `/__fourfold/htmx.js` with eval, script tags in swapped content, and inline indicator styles turned off. Pages load it with an explicit `<script>` tag. |
| Cost of rendering on the script thread | Within M2's numbers: 130–140 µs for a trivial `render`, 240–340 µs for a typical one (M2: 130–150 and 240–300). |

Measurements were taken on the development Mac (macOS 13.6, Apple clang 15), with the `release` preset unless noted.

---

## Templates

### Contexts

Each field is classified at compile time by where it is. The value is then written as follows:

| Context | Example | How a value is written |
|---|---|---|
| Text | `<p>{{msg}}</p>` | `& < > " '` as character references; rendered HTML (`Html`) as is |
| RCDATA | `<title>{{t}}</title>`, `<textarea>{{m}}</textarea>` | escaped; `Html` is a render error |
| Quoted attribute | `title="{{tip}}"`, `class="a {{b}}"` | escaped; if the field is the whole value and the value is null, the attribute is left out |
| Boolean attribute | `checked="{{agreed}}"` | the attribute alone if the value is truthy (a non-zero number, a non-empty string), else left out |
| URL, at the start | `href="{{link}}"` | the URL rule (below), then normalized and escaped |
| URL, later | `href="/u/{{id}}?p={{n}}"` | percent-encoded as one component (`/`, `:`, `?`, `&`, `=` included) |
| Script | `<script>go({{cfg}})</script>` | JSON, with `< > & / ' \`` U+2028 and U+2029 also escaped as `\uXXXX` |

- **Which attributes are URLs.** The names are matched as `html/template` matches them (`href`, `src`, `action`, `formaction`, `cite`, `poster`, `data`, …, and any name containing `src`, `uri` or `url`), plus HTMX's (`hx-get`, `hx-post`, `hx-put`, `hx-patch`, `hx-delete`, `hx-push-url`, `hx-replace-url`, `sse-connect`, `ws-connect`). A `data-` prefix is ignored, so `data-hx-get` counts.
- **The URL rule.** Leading spaces and control characters are dropped, as browsers drop them. Then a URL fails if it contains any other control character (browsers silently delete tabs and newlines, so `java\tscript:` is `javascript:` to them), or if it has a scheme other than `http`, `https` or `mailto`. A URL is relative if no `:` comes before the first `/`, `?` or `#`. A URL that fails becomes `about:invalid#fourfold`, with a dev warning.
- **A field "at the start"** has only whitespace before it in the value. Anything else in front, even `java`, makes it a later part, which is percent-encoded, so its `:` can't complete a scheme.
- **Values by type.** Numbers use MiniScript's own formatting. Null, and a field the map doesn't have, render as nothing (`null` in a script). Maps and lists are errors outside a script, and `Html` is an error anywhere but text. A dotted path (`{{user.name}}`) through a null or a missing key is null, and through anything else that isn't a map is an error.

### Compile errors

Every rule below is a compile error, with a message that says what to do instead.

| Refused | Why |
|---|---|
| A field in an unquoted attribute value, a tag name or an attribute name | The value could end the attribute or the tag |
| A field right after `<` | `<{{x}}` with `x = "script"` would make a tag |
| A field in a comment, `<style>`, a `style` attribute, `srcset`, `srcdoc`, `<base>`, or a `<meta http-equiv>` tag | CSS, nested HTML, page-wide URL changes and redirects: contexts with no safe escaping in v1 |
| A field in `xmp`, `iframe`, `noembed`, `noframes` or `noscript` | Raw-text elements, where escaping doesn't apply |
| A field in `hx-vals`, `hx-headers`, `hx-trigger` or `hx-vars` | HTMX can evaluate these as JavaScript |
| A field in a `<script>` inside a string, a template literal (including `${…}`), a regular expression or a comment | In a script, a field is a whole JSON value (see below) |
| A field in a `<script>` whose `type` isn't JavaScript, a module, JSON, `ld+json` or an import map | Its content is some other language |
| Any field inside `<svg>` or `<math>` | Foreign content parses differently (CDATA, no raw text, integration points); refused rather than modeled |
| Any `on*` attribute (`onclick=`), any `hx-on*` attribute, `hx-vars`, and `js:`/`javascript:` values in `hx-vals` or `hx-headers` | Inline JavaScript, which the CSP blocks anyway: an error is clearer than a page that silently doesn't work |
| A `nonce` attribute | The engine adds the nonce |
| `<meta name="htmx-config">` | It could turn htmx's eval and script-tag settings back on |
| `<script>` content containing `<!--` or `<script` | They switch the tokenizer into its escaped and double-escaped script states, where `</script>` may not end the element |
| In `<svg>` or `<math>`: an HTML element that ends it early in browsers (`<p>`, `<div>`, …), `<script>`, `<style>`, `<foreignObject>`, raw-text elements, and tags inside `<title>` or `<desc>` | The same reason: so the tokenizer's view of where foreign content ends matches the browser's |
| `<plaintext>` | Nothing after it would be HTML |
| A page that doesn't start (after comments and whitespace) with a doctype or `<html>` | Pages must be complete documents |
| `<html>`, `<head>`, `<body>` (or their end tags), or a doctype in a fragment | Fragments must not be documents |
| An unclosed tag, attribute value, comment, `<script>`, raw-text element or `<svg>` | A fragment must end where it began, in text, to be included safely |
| A malformed field or include, a missing fragment, an include cycle, `x.html` and `x.md` both in `frags/` | |
| A NUL byte | |

The sample apps (`examples/` and `tests/apps/`) needed no changes for these rules, except that pages written as bare fragments (`<p>{{x}}</p>` in `pages/`) became errors. The tests' own fixtures were full of those; real pages aren't.

### Fields in `<script>`

The HTML side is simple: a `<script>`'s content ends at `</script`, and `<!--`/`<script` inside it are refused. The JavaScript side needs a small lexer (`src/template/js_lexer.cpp`) that follows strings, template literals (with nested `${…}`), regular expressions and comments, so that a field is allowed only where an expression can go.

The hard part is `/`. After `)` or `}`, it may start a regular expression (`if (a) /re/.test(s)`) or be a division (`(a) / b`), and only a full parser knows which. `html/template` guesses from the previous token. **This lexer follows both readings** wherever they differ, keeping each reading that's still possible (a regular expression can't span lines, so most die at the next line end), and a field is allowed only if every live reading puts it in code. The readings are deduplicated and capped at 16. This found real ambiguity in testing: `if (a) /"/.test(s); y = "{{x}}"` looks like code under the division reading, but is inside a string under the other, so it's refused.

As defense in depth, values in scripts are written as JSON that also escapes `/`, `` ` `` and `'`. So even if the lexer misjudged a context, a value couldn't end a regular expression, a template literal or a single-quoted string.

### Includes

`{{> name}}` is allowed only in text. Each fragment is parsed on its own, starting in text, and must end in text. An include splices the fragment's parsed parts in, so contexts stay consistent without reparsing. Fragments' includes are resolved first, depth first, with cycles reported along their chain (`include cycle: a → b → a`). A field's file and line are where it's written, so an error or warning in an included fragment names the fragment.

### Missing and unused fields

Rendering reports a field with no value (`{{title}} wasn't given a value`) and a value with no field (`"typo" was given a value, but … has no {{typo}} field`), but not for pages with no code, whose fields are meant to be empty. The warnings go to the `Observer` (`renderWarnings`). `DevServer` prints each once per app generation. `DevTools` keeps them for the generation and shows them in a small, dismissible panel on open pages, including the page whose request produced them (they come in the `hello` event, since that request finished before its page's event stream opened).

### A real site

The HTML of miniscript.org (68 files; kept locally in the git-ignored
`tests/local/`, since it isn't ours to publish) was compiled as a Fourfold app:

| Refused | Count | Verdict |
|---|---|---|
| Pieces of pages in `pages/` (tab contents and help text loaded by script) | 49 files | Correct; moved to `frags/`, where all 49 parse |
| Inline event handlers | 58, in 21 files | Correct: the CSP would block them. The real conversion cost; the error message now shows the `data-` attribute and `addEventListener` way, and [converting.md](../user/converting.md) shows a delegated listener for the common case |
| `{{` in a code sample (MediaWiki markup) | 1 | Needs `\{{`, as the message says |

Inline scripts (6), external scripts (17), `style` attributes (397) and the one form passed unchanged. The Mini Micro page was then converted properly: its tabs, which `script.js` fetched and set with `innerHTML`, became includes; its tab links use `data-tab`; and its release facts and download URLs became fields filled by `index.ms`.

One finding that isn't about templates: 57 of the site's links end in `.html`, and Fourfold serves pages only without the extension, so `/earn.html` is a 404. **Decided:** a GET for a page by its file name now redirects to its URL (`/earn.html` to `/earn`, `/docs/index.html` to `/docs/`, keeping the query), like the trailing-slash redirect, with 302 for now. It applies only when no route has that exact path and the page exists.

That led to a new compile-time check, **the links pass** (`src/compiler/pass_links.cpp`). The parser records every literal URL in a link attribute (`href`, `src`, `action`, `formaction`, `poster`, `<object data>`, and HTMX's URL attributes), and the pass warns about each local one that no route serves:

- A page's relative URLs resolve against its folder, including those of the fragments it includes. For fragments on their own, the 404 and 500 pages, and the Markdown layout, which can appear at any URL, only URLs from the root are checked.
- A link to `x.html` or `x/index.html` works (by redirecting), but the warning says to write `x` or `x/`.
- A link to a file that's there but not served says so (an unknown type, or a page's `.ms` code).
- A link repeated in one file with different queries is one warning, with a count.

The Markdown pass now uses the same resolver (`src/compiler/links.cpp`). On the copy of miniscript.org, besides the `.html` links, it found:

- a stylesheet missing from the site's own files (`MiniMicro/dlc/css/layout.css`);
- links to `.minidisk` and `.tar.gz` downloads, which Fourfold doesn't serve (not on the content-type list);
- links into `tryit/` and `wiki/`, which are PHP or served elsewhere.

---

## Markdown

- **md4c 0.5.2**, vendored (`third_party/md4c/`): its parser and HTML entity table only, with `MD_DIALECT_GITHUB | MD_FLAG_NOHTML`. Raw HTML in Markdown is shown as text.
- **Our own renderer** (`src/markdown/markdown.cpp`), on md4c's callbacks:
  - It wraps the output in `<div class="markdown">` or `<div class="markdown comment">`.
  - Link and image URLs are entity-decoded, then put through the template URL rule, so `[x](&#106;avascript:…)` is caught. A link or image that fails becomes plain text.
  - **Comment profile:** headings become paragraphs; images become `rel="nofollow ugc"` links to the image; links get `rel="nofollow ugc"`; nothing gets an `id`.
  - **Full profile:** headings get GitHub-style ids (`#hello-world`, then `-1`, `-2` for repeats), and the first `#` heading is the page's title.
  - Task-list checkboxes are `disabled`. A code block's language becomes `class="language-x"` only if it's made of `[A-Za-z0-9_+-]`.
- **Markdown pages** (`pages/x.md`) are rendered at compile time. A link to another `.md` file is rewritten to its route, keeping any `#anchor` (`setup.md#x` becomes `/setup#x`, and `docs/index.md` becomes `/docs/`). A relative or site-absolute link or image to a route that doesn't exist is a warning. The page is served in `frags/layout/markdown.html` (which must have `{{content}}`; `{{title}}` is optional) or, without one, a minimal built-in page.
- **Markdown fragments** (`frags/x.md`) work with `frag("x")` and `{{> x}}`, and take no value.
- **`markdown(text, profile)`** from code defaults to `"comment"`, and is capped at 256 KB.
- **`markdown.css`.** A page holding rendered Markdown links `/markdown.css` if the app has `pages/markdown.css`, else the built-in `/__fourfold/markdown.css`, at the end of its `<head>`.
- **Footnotes aren't supported.** md4c has no footnote extension. Adding them means a footnote pass of our own. That's left until someone needs footnotes; the overview's profile table marks them deferred.

---

## CSP, HTMX, and responses

- **Nonce.** `Engine::handle` takes 16 bytes from the OS's generator (`platform/random`: `arc4random_buf` on macOS, `getrandom` on Linux) for each request, as base64. The renders for that request add `nonce="…"` to every `<script` start tag they write: the page, every `frag()`, the error and 404 pages, and the dev tools' client script.
- **The engine's own files.** `/__fourfold/` is reserved and handled after Admit, as `/__dev/` is. `/__fourfold/htmx.js` serves htmx 2.0.4 (`third_party/htmx/`, 0BSD license) followed by `src/engine/pipeline/assets/htmx-config.js`, which sets `allowEval`, `allowScriptTags` and `includeIndicatorStyles` to false, keeps `selfRequestsOnly`, and makes 422 responses swap (for `reshow`, M7). `/__fourfold/markdown.css` is the default stylesheet. Both are compiled into the program (`fourfold_embed`, `cmake/Embed.cmake`) and cached for a day.
- **Rendering moved to the script thread.** Field values are MiniScript values, so a `render`'s page is filled on the script thread, by `ValueFields` over the returned map, and `HandlerOutcome::Render` now carries the finished page. Pages with no code, Markdown pages and error pages are still rendered on the I/O thread, with no MiniScript involved.
- **`Html` handles.** `frag()`, `markdown()` and `unsafeHTML()` return handles, checked against a live set before use (like M2's outcome handles). A map dressed up as HTML (`{"html": "<b>"}`, with or without an `__isa`) is just a map: an error in a field, and never markup.
- **Fragment responses.** A handler that returns a fragment gets `text/html`, not decorated by the dev tools, since it goes into a page that already has them. A fragment containing `<script>` (from a template, or `unsafeHTML` text containing `<script` in any case) is refused as a response, or as `status(...)`'s body, because its script couldn't have the page's nonce. In a page's field it's fine.
- **HTMX.** `req.htmx` is true for `HX-Request: true`. `redirect(url)` answers an HTMX request with 200 and `HX-Redirect`, so HTMX loads the new page rather than swapping the redirect's target into the old one.

### The cost

`bench_script` now renders real templates for its pages (the typical one has a `<script>`, so a nonce too). Two runs:

| | Latency, one at a time | 1 in flight | 100 in flight |
|---|---|---|---|
| trivial | 132–141 µs | 7,200–7,600 /s | 7,100 /s |
| typical | 238–259 µs | 3,900–4,100 /s | 6,300 /s |

These are within M2's ranges (trivial 130–150 µs, typical 240–300 µs). A first run measured 336 µs for the typical page, which looks like warm-up rather than cost.

---

## Fuzzing and time

`tests/fuzz/fuzz_test.cpp` runs with the other tests, from a fixed seed:

- **Templates:** 3,000 mutations of a seed corpus, built by inserting tokens such as `{{x}}`, `<!--`, `</script>`, quotes, `${`, `/`, NUL and U+2028, by deletion, and by repetition. 1,157 of them parse. Each one that parses is rendered with a hostile value in every field. The check is that the value's `"'<>` never appears unescaped, and that no attribute starts with `javascript:`.
- **Markdown:** 20,000 mutations plus 2,000 random byte strings, in both profiles. The check is that every tag and attribute in the output is one the renderer writes, with only `http`, `https`, `mailto` or relative URLs. In comments, it also checks that there are no images, ids or headings.
- **Time:** adversarial inputs are timed at 1× and 4× their size, and must take less than 8× the time, which allows for noise; linear is 4×. Every one comes out between 2.7× and 4.5×. In the release build:

| Input | Size | Time | ×4 input |
|---|---|---|---|
| Template: `{{` repeated | 32 KB | 4.2 ms | ×4.1 |
| Template: 40,000 fields in attributes | 160 KB | 12 ms | ×3.8 |
| Template: `)/` repeated in a script (forks the lexer's readings) | 32 KB | 19 ms | ×3.9 |
| Template: nested `<svg>` | 176 KB | 6.8 ms | ×3.7 |
| Rendering: escaping a 800 KB value, twice | 800 KB | 44 ms | ×4.5 |
| Markdown: `[` repeated | 20 KB | 1.4 ms | ×3.2 |
| Markdown: `**_a` repeated | 80 KB | 2.5 ms | ×4.5 |
| Markdown: `>` nesting | 20 KB | 1.4 ms | ×4.3 |
| Markdown: a table with 15,000 columns | 60 KB | 0.4 ms | ×4.1 |

All 30 cases are printed with `FOURFOLD_FUZZ_TIMES=1`. The slowest per byte is JavaScript inside a template, at about 1.6 MB/s: a 100 KB inline script would add about 60 ms to a compile. Large scripts belong in `.js` files anyway.

**What fuzzing found:**

- **A crash.** md4c reports an indented code block's language as an empty attribute with no substring arrays, and the renderer read them. Fixed, with a unit test.
- **Under ASan:** a dangling `string_view` in the Markdown pass (segments of a temporary path). Also fixed.

No libFuzzer here: Apple's clang doesn't ship it. `fuzz_template` and `fuzz_markdown` build only where it's available (`FOURFOLD_HAVE_LIBFUZZER`), so open-ended fuzzing is still to be run. The whole suite passes under the `asan` preset.

---

## Other changes

- **Components:** `template` rewritten; `markdown` new (DEPS base, template; LIBS md4c). `compiler`, `script` and `pipeline` gained the dependencies they use, and `pipeline` now depends on `platform` for its random source.
- **`CompiledApp`:** fragments in `TemplateSet`, `markdownPages`, `markdownLayout`, `hasMarkdownCss`, and `RouteKind::Markdown`. `describe()` lists fragments and Markdown titles, so the goldens show them. There's a new golden, `tests/apps/markdown-site`.
- **`ScriptHost::run`** takes `RenderOptions`. `HandlerOutcome` has a `Fragment` kind, `hasScript`, and `warnings`.
- **`EngineHooks::decorate`** takes the nonce. `Observer` has `renderWarnings`.
- **Samples:**
  - `examples/guestbook` is the overview's guestbook, with fixed sample entries, some of them hostile.
  - `examples/markdown-site` is a site made only of `.md` files.
  - An end-to-end test runs the guestbook through `DevServer`: a nonce on every script, hostile entries kept as text, an HTMX post answered with an escaped `<li>`, and a plain post redirected.

---

## Deferred

| Item | Why, and when |
|---|---|
| More real-world templates with fields | The miniscript.org port had no fields to begin with; a site built on another template system (with forms and inline SVG icons) would test field placement harder |
| Footnotes in the full profile | md4c has none; needs a pass of our own |
| Fields in Markdown | Still unresolved (overview §4); v1 Markdown has none, and `{{` in it is text |
| Turning off `markdown.css` injection, and serving `.md` source (`/about.md`) | Config settings, which arrive with `config` in M6 |
| `<style>`, `style=` and CSS fields | No CSS escaping in v1; refused |
| Fields inside `<svg>` and `<math>` | Refused; could be allowed for plain attributes once foreign content is modeled |
| libFuzzer runs | Need a libFuzzer-capable compiler |
| CSP violation reports in the overlay (overview §8) | Needs a report endpoint; with production's logging (M11) |

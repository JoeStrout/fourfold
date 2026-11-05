# Fourfold: M7 Results — Forms and abuse protection

*Status: 2026-11-05, at the end of M7 ([milestones.md](milestones.md)). Records what was built for declared forms, uploads and the spam defenses, the choices the milestone left open, and what's left for later. No MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| Can form declarations be read at compile time and enforced before the handler runs? | **Yes.** `form.x` is read during the compile-time discovery run, like `access =`, and becomes a `FormSpec` on `post.x`'s route. The Parse stage normalizes, converts and validates against it, and drops undeclared fields. |
| Do failed forms come back with errors and input, with and without JavaScript? | **Yes.** `reshow` answers HTMX with the fragment (422, `HX-Retarget`), and a plain post with a redirect to the page, whose `formState` shows the errors and input once. |
| Are uploads checked by content? | **Yes**, against an allowlist of types Fourfold can recognize; the name and claimed type are ignored. |
| Are forms checked against templates? | **Yes.** Mismatches are compile warnings, and the declaration's rules become `required`, `maxlength` and the like on the inputs. |
| Do rate limits work across restarts and processes? | **Yes.** `RateStore` keeps sliding-window counters in `state.db`; concurrent hits from two connections count exactly. |
| Behind a proxy, are visitors told apart, and forged addresses ignored? | **Yes**, with `config.proxy.trust`; tested end to end. |

---

## What was built

- **`base`:** `toNfc` (over utf8proc), `toValidUtf8`, `utf8Length`; `sha256` and `leadingZeroBits` for the puzzles (BLAKE2b stays everywhere else).
- **`third_party`:** utf8proc 2.12.0, vendored (chosen with the user over linking the system's copy, or writing our own tables).
- **`platform`:** `AddressRange` (an address or CIDR range, IPv4-mapped addresses as IPv4).
- **`compiledapp`:** `FormSpec` and `FormField`; `RateRule` and `parseRate`; on `RouteEntry`, `limits` and `limitSources`, `minSeconds`, `maxAgeSeconds`, `challenge`, `form` and `pageUrl`; `CompiledApp::forms` (for `formState`) and `tooManyPage` (`pages/429.html`).
- **`template`:** the parser records forms that post (`FormStart`, `FormEnd`) and their named controls (`FormControl`), and a fragment's root `id`. `CompiledTemplate::forms()` and `annotateForms` let the compiler add attributes and mark forms for a puzzle. `render` writes the timing token (`_t`) into every post form, and a puzzle (`_pow`) into marked ones, linking the solver at the end of `<head>`. Forms with `hx-post` and no `method="post"` now get the CSRF field too.
- **`compiler`:** `pass_limits.cpp` (`code/limits.ms`, rules combined per route, unmatched-rule warnings) and `pass_forms.cpp` (declarations into `FormSpec`s; forms in templates checked and annotated).
- **`script`:** `form` is a map the engine creates, like `get` and `post`, and discovery reads it. `req.form` (typed, with a declaration), `req.errors`, `req.files` (each with its bytes as `RawData`), and `req.ip`. `formState`, `reshow` and `ratelimit` (`modules/forms.cpp`), and `file.send`. The session's engine part reaches the run (`SessionInput::engine`), and a changed one comes back in `SessionResult`. A file naming `formState` or `reshow` uses the session. Outcomes can carry extra headers.
- **`state`:** `RateStore`: counters, used puzzles, sweep, reset, and a listing for the dev tools.
- **`config`:** `config.limits` (`anonymous`, `signedIn`, `challengeBits`) and `config.proxy.trust`.
- **`policy`:** `checkRates`, `checkTiming` and `checkChallenge` after access and CSRF; `formTimeToken` and `newChallenge`; `needsStore`, so the engine runs the gate on a worker only when it counts something.
- **`pipeline`:** `pipeline/forms.h`: `readSubmission` (url-encoded and multipart, under limits), `detectContentType`, `normalizeFormText`, `countLinks`, `checkForm`. `clientAddress`. The engine reads the body before the gate, holds a route's body to its declared uploads, runs the gate (on a worker when it counts), checks the form, and answers refusals with 429 (`Retry-After`, `pages/429.html`) or 403 pages. The proof-of-work solver is served at `/__fourfold/challenge.js`, and appended to `/__fourfold/htmx.js`. The session's engine part is saved when a run changed it, and a new anonymous session is made for it.
- **`httpserver`:** a multipart body may be up to `maxUploadBodyBytes` (33 MB); others stay at 1 MB.
- **`devtools`:** the access report shows each route's limits, timing check, puzzle and form (or that a post handler has none); `/__dev/limits` lists counters and recent counts and refusals, with a reset button (POST, same origin only); a `limitsCounted` Observer event.
- **`hosts/cli`:** `AppServer` opens the `RateStore`, sweeps it hourly, and logs the visitor's address as the engine sees it. `fourfold serve` warns when no proxy is trusted.
- **Samples:** the guestbook declares its form (`frags/signForm.html`, `formState`, `reshow`), has `code/limits.ms` and `pages/429.html`, and a photos page with an upload, `file.send` and a puzzle.

## Choices made here

- **`form.x` is for `post.x` only.** A declaration without a `post.x` is a compile error. GET forms (search boxes) keep reading `req.query`; declaring those can come later.
- **What `req.form` holds with a declaration:** every declared field, so no `hasIndex` is needed. Text that's empty is `""`; a number, date or choice that's empty or invalid is `null`; any field that broke a rule is `null`, so a handler can't use a bad value by mistake. A checkbox is 1 or 0; `choices` is a list in the options' order. Files are in `req.files`, not `req.form`.
- **What `formState` gives a template:** text as typed, a checkbox as 1 or 0, and a `choice` or `choices` field as a map from each option to 1 or 0, so `checked="{{mood.happy}}"` re-checks radio buttons and checkboxes without logic in the template.
- **`reshow` without JavaScript** keeps the state in the session's engine part (under `"forms"`, by `"pages/x.ms#name"`), so app code can't read or forge it, and a visitor without a session gets one. It redirects to the page beside the handler's file, never the `Referer`; a file with no page makes `reshow` an error there.
- **Multipart names are read as browsers write them** (the HTML spec): `%22`, `%0D` and `%0A` decoded, and a backslash taken literally, since old browsers send Windows paths. File names lose any folders and control characters; apps are told not to use them as paths.
- **Upload types are checked by magic numbers** for PNG, JPEG, GIF, WebP, PDF and ZIP; `text/plain` means valid UTF-8 without binary control characters. Other types can't be declared, since they couldn't be checked. `maxKB` defaults to 1024, at most 16384 (the file size cap).
- **`file.send`** chooses the type from the extension, from a short allowlist: images, text, audio and video inline; PDF, ZIP, CSV, JSON and Markdown as downloads; anything else (HTML and SVG included) as `application/octet-stream`, downloaded. `X-Content-Type-Options: nosniff` is on every response already.
- **Rate counting:** sliding windows estimated from two fixed ones, in SQLite with no in-memory cache (fast enough; a cache can come with M11). A request is counted only if access and CSRF pass, and refused requests count nothing, so a flood doesn't keep a visitor locked out after it stops. The routes under one `limits.ms` rule share its counter.
- **Which requests count:** the defaults, every method but GET, HEAD and OPTIONS; a route's own rules, every request to it, GET included (so `"/api"` limits API reads too).
- **Defaults:** 60 a minute per address group anonymous, 300 a minute per user signed in, set in `config.limits`. `per session` falls back to the address group for visitors with no session.
- **Timing tokens** go in every post form (not only those posting to a route with `minSeconds`), signed and bound to the visitor's CSRF cookie. They aren't single-use (still open in the overview). Too soon is a 429 with `Retry-After`; no token or a bad one is a 403.
- **Puzzles:** SHA-256 (which browsers can compute) over `puzzle~n`, with the difficulty in the puzzle, signed and bound to the CSRF cookie, good for 24 hours and once only. The compiler marks the forms that post to a challenge route, so other forms carry none, and visitors' devices don't solve puzzles nothing will check. Difficulty is 16 bits by default and 4 in dev, from config, not raised automatically (still open). The solver starts on the visitor's first interaction with the form, and holds back a submit until it's done.
- **Trusted proxies** are an app config setting (`config.proxy.trust`), as overview §8 has it, not a server-config one; reloadable. `X-Forwarded-For` is read from the right; garbage stops the walk at the last address the proxies vouch for. `Forwarded` isn't read.
- **The request inspector** doesn't exist yet, so `/__dev/limits` lists the latest counted and refused requests instead.
- **Body limits:** forms 1 MB (1,000 url-encoded fields, 100 multipart parts); a route with file fields may add its fields' `maxKB` (plus 64 KB for multipart overhead); the HTTP server allows 33 MB for any multipart body before the engine sees the route.

## Tests

- `tests/base/`: NFC, UTF-8 repair and length; SHA-256 vectors (including both padding edges); leading zero bits.
- `tests/platform/socket_test.cpp`: address ranges.
- `tests/state/rate_store_test.cpp`: counting, the sliding window, all-or-nothing checks, exact counts from 8 threads on two connections, puzzles once only, sweep and reset.
- `tests/engine/policy/policy_gate_test.cpp`: `parseRate`; rate limits default and per route, per address and per user, not counted when refused by access; the timing check (too soon, forged, another visitor's, too old); proof of work (wrong, reused, another visitor's, too easy, expired, forged, malformed); the order of the checks.
- `tests/engine/pipeline/forms_test.cpp`: reading bodies, malformed and oversized ones refused; content sniffing; normalization; every type and rule; tampered submissions (undeclared fields, bad choices, forged file types); `clientAddress`.
- `tests/compiler/forms_test.cpp`: declarations into `FormSpec`s; warnings for undeclared inputs and missing ones; added validation attributes; each kind of mistake in a declaration or `limits.ms`; limits combined per route; puzzles only in forms that post to a challenge route; `pages/429.html`.
- `tests/config/app_config_test.cpp`: `config.limits` and `config.proxy`.
- `tests/fuzz/fuzz_test.cpp`: mutated url-encoded and multipart bodies read or refused within limits, then checked; linear time on many parts, nothing but delimiters, and many fields.
- `tests/hosts/cli/forms_test.cpp`, end to end (**the exit criteria**):
  - the guestbook sample: too soon, and no timing token, refused; error redisplay with HTMX (422, in place, input kept) and without (redirect, shown once, to that visitor only); a check from the handler; a flood of `/sign` throttled at 5 a minute with the app's 429 page;
  - behind a trusted proxy, visitors told apart by forwarded address, and a forged address at the left of the header ignored; IPv6 visitors counted by /64;
  - from an untrusted peer, a forged `X-Forwarded-For` ignored entirely;
  - tampered submissions dropped or refused; uploads saved, a forged PNG refused, an oversized body refused; `file.send` inline, as a download, and an HTML file never as HTML;
  - the timing check, a puzzle solved as the browser does and refused the second time, and `ratelimit` from code.
- `tests/hosts/cli/app_server_test.cpp`: the older guestbook test waits out `minSeconds`.

All 21 suites pass with the `dev` and `asan` presets.

## Still to check by hand

- **The solver in real browsers**, on a phone especially: that 16-bit puzzles solve in well under a second, and that a submit before the answer is ready goes through once it is. (In dev mode, open http://localhost:8090/photos in `examples/guestbook` and upload a photo.)
- **The guestbook with JavaScript off**: the errors and input come back after a failed post.

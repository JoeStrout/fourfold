# Fourfold: M9 Results — Outbound HTTP, secrets, webhooks, and app mail

*Status: 2026-11-12, at the end of M9 ([milestones.md](milestones.md)). Records what was built for outbound requests, secrets, webhooks and app mail, the choices the milestone made, and what's left for later. M9 is the first half of what was planned as one milestone; jobs and push are M9.1. No MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| Can a visitor steer the app's requests to an internal address? | **No.** Every address is checked as it's connected to, so the metadata address, loopback (by name, by number, IPv4-mapped), private ranges, a name pointing at one, DNS rebinding and a redirect to one are all refused, before any connection. |
| Can a secret leak? | **No path found.** App code holds only a handle with the secret's name. Printing it gives a placeholder; a page, `json()`, `data`, the session and a mail refuse it; the log has neither secret values nor query strings; a request (or a redirect) to a host the secret isn't bound to isn't sent. |
| Do webhooks work without opening a CSRF hole? | **Yes.** `webhook.x` is a separate map: its routes, and only those, skip the CSRF check, and get the raw body to check a signature with `hmac.verify`. |
| Can app mail be made into a spam relay? | **No.** `mail.send` goes to a user (by id) or an address `code/outbound.ms` lists; there are no headers to inject; the app is limited per hour. |
| Does the weather sample work? | **Yes**: tested against a local stand-in, and by hand against `api.weather.gov` over HTTPS. |

---

## What was built

- **`outbound`** (new component, libcurl): `OutboundClient` (`outbound_client.h`): URLs and headers assembled from strings and `SecretRef`s; the allowlist and the config's cap; one exchange per hop, redirects followed and checked by the client; limits on size (after decompression), time and rate; `hmacSha256` and `verifyHmac`; `parseOutboundUrl` and `redirectRefusal`. `address_rules.h`: `blockedAddressReason`, every reserved range, IPv6 forms that carry IPv4 judged by it. `Secret` and `SecretMap` moved here from `config`.
- **`compiledapp`**: `OutboundPolicy` and `HostRule` (`outbound_policy.h`), `parseHostRule`, `parseMailRule`; `HandlerRef::Kind::Webhook` and `RouteEntry::webhook`; `CompiledApp::outbound` and `mailTemplates`.
- **`compiler`**: `pass_outbound.cpp` (`allow` and `mailTo` from `code/outbound.ms`, with warnings for `"*"`); webhooks routed beside `get` and `post`, `post.x` with `webhook.x` an error; `code/mail/*.md` compiled as Markdown templates; a `limits.ms` rule with `minSeconds` or a challenge over a webhook warns.
- **`script`**: the `webhook` map, discovered like `get` and `post`; secret handles (`SecretHandle` in the bridge), refused by `toDatum` and template fields; `http.cpp` (the `http`, `secrets` and `hmac` modules); `mail.cpp` (`mail.send`); `req.rawBody` and `req.json` for webhooks; `RunContext::outbound` and `mailer`; `AppMailer`; a second worker pool, `network`, and `startNetworkWork`; each run's start time, for `timeLeft`.
- **`config`**: `config.outbound` (`enabled`, `allow`, `allowAddresses`, `perMinute`, `maxResponseKB`, `timeoutSeconds`); `mail.appMail` and `mail.appPerHour`; secrets with `verify`, and `*.` hosts; an engine secret must have neither `hosts` nor `verify`.
- **`base`**: `isJsonMediaType`.
- **`policy`**: a POST to a webhook skips CSRF, the timing check and the puzzle.
- **`pipeline`**: the engine makes an `OutboundClient` with each config (pinned per request), logs each call (`outbound`), and gives runs an `EngineMail` (users by id, the hourly limit in the `RateStore`, the config's `Mailer`, `mail` log lines). Webhook requests carry the body and parsed JSON.
- **`observer`**: `outboundCall`.
- **`devtools`**: `/__dev/outbound` and `/__dev/outbound.json` (hosts, `mailTo`, secrets by name with uses and hosts, webhooks, the latest calls and refusals); "outbound" in the corner menu.
- **`mail`**: `MailMessage::bcc`, sent as envelope recipients (SMTP) or Postmark's `Bcc`; never in `formatMessage`'s headers; at most `kMaxBcc` (10).
- **`state`**: `RateCheck::cost`, so one request can count as several.
- **`hosts/cli`**: the dev tools' secret list (names, uses, hosts) from the config serving now.
- **Sample:** `examples/weather`: the National Weather Service forecast (two requests, the second to a URL from the first), a contact form mailing the owner, and a signed webhook. Its `secrets.ms` (a dev-only webhook key) is kept in git, unlike other samples' config.

## Choices made here

- **Split M9** (decided with the user): the security core here, jobs and push in M9.1.
- **App mail to users and an allowlist** (decided with the user): see overview §6, [Email from app code](overview.md#email-from-app-code-decided-2026-11-12-m9). Templates are Markdown, in `code/mail/`; `mail.send` returns null on success.
- **Blind copies, no CC** (decided with the user): `mail.send(..., {"bcc": [...]})`, up to 10, each a user's id or an address in `mailTo`. They're envelope recipients only (SMTP `RCPT TO`, Postmark's `Bcc` field), never a header. A repeat of `to`, or of another copy, is dropped. Every recipient counts against `appPerHour`, in one all-or-nothing count (`RateCheck::cost`), and one refused recipient means nothing is sent. CC would show users' addresses to each other, so there's none.
- **Check on connect, not resolve and pin.** libcurl's open-socket hook sees every address it's about to connect to, including each one it tries for a name with several, so the check and the connection can't be separated. The hook refuses a blocked address and libcurl moves on to the name's next one, so a name with both public and private addresses reaches only the public ones (the plan had refused such names outright). Proxies from the environment and `.netrc` are off.
- **IPv6 carrying IPv4**: mapped (`::ffff:`), compatible (`::a.b.c.d`), NAT64 (`64:ff9b::/96`) and 6to4 (`2002::/16`) are judged by the IPv4 address inside, rather than all refused, so a server on an IPv6-only network with NAT64 can still reach public IPv4 hosts. Teredo and local-use NAT64 are refused.
- **Numbers that only look like hosts** (`0x7f.1`, `2130706433`, `127.1`, a numeric top-level label) are refused as URLs: only dotted-decimal IPv4 counts as an address.
- **A secret in a URL** goes only in the path or query, percent-encoded (so a value with `&` can't add a parameter). Secrets are marked while the URL is parsed, so none can land in the host.
- **Redirects**: the client follows them, at most 5, each hop checked against the allowlist, the cap and the address rules; HTTPS to HTTP is refused. 303, and 301 or 302 after a POST, become GET without the body. To another host, `Authorization` and `Cookie` without secrets are dropped (as browsers do), and any header with a secret not bound to the new host fails the request.
- **A response of any status is a map**; only a refused or failed request is an error value. `body` is text, or null with `raw` (`RawData`) for bytes that aren't UTF-8 text; `json` is parsed when the response says it's JSON. Header names go out as the app wrote them.
- **A separate network pool** (8 threads) for `http` and `mail`. With four slow APIs on the shared 4-thread pool, sessions, rate counting and queries would all have waited. A call's deadline is fixed when it's made, so waiting for a thread uses the run's time.
- **`hmac.verify`** is SHA-256 only (SHA-1, for a few older providers, can follow); hex or base64; GitHub's `sha256=` prefix accepted. A secret with `hosts` may verify too; an engine-only secret never. App code never sees an HMAC.
- **Webhooks** also get `req.json`. They skip the timing check and puzzle as well as CSRF; access rules and rate limits apply.
- **Limits**: 120 requests a minute per app (counted in memory, so a restart or config change resets them), 5 MB, 10 seconds and the run's own budget. App mail: 100 an hour, counted in the `RateStore`.

## Tests

- `tests/outbound/outbound_test.cpp` (new): every blocked range and the IPv6 forms (and public addresses allowed, and `allowAddresses`); URL parsing (schemes, credentials, numeric tricks, encoding); host rules and `mailTo` rules; HMAC-SHA-256 against RFC 4231 and `verifyHmac` in every form; then, against a fake server (`tests/support/fake_http.h`, new), with names resolved where each test wants:
  - a request with a secret in a header and in the query: sent encoded, logged without it;
  - **SSRF**: loopback by name, by number, `[::1]`, IPv4-mapped (as a literal and behind a name), the metadata address (literal and by name), a private address, a name whose every address is private, `0.0.0.0`: all refused, nothing reaches the server;
  - **DNS rebinding**: the same name allowed, then resolving to the metadata address, refused; a name with a blocked and an allowed address connects only to the allowed one;
  - the allowlist: no `outbound.ms`, another host, HTTPS only, another port; the config's cap; outbound turned off;
  - secrets only to their hosts; verify-only and engine-only secrets never sent; never in a URL's host; no header injection; no `Host` header;
  - redirects to a private address (by name and number) and off the list refused; relative redirects; 303 and 307; a loop; a secret refused on a redirect to another host, a plain `Authorization` dropped;
  - limits: size, a deflate bomb (4 MB of zeros in a few KB) and a small deflated body, time, the run's time left, and the rate.
- `tests/compiler/outbound_test.cpp` (new): `outbound.ms` read, `"*"` warnings, each kind of mistake; webhooks routed, the `post`/`webhook` collision, the `limits.ms` warning; mail templates, with includes.
- `tests/mail/mailer_test.cpp`: blind copies as SMTP recipients and Postmark's `Bcc`, never in the message's headers; bad or too many refused. `tests/state/rate_store_test.cpp`: a request that counts as several.
- `tests/config/app_config_test.cpp`: `config.outbound`, `verify` and `*.` secrets, app mail settings, mistakes.
- `tests/hosts/cli/outbound_test.cpp` (new), end to end:
  - `http` from app code with a secret header; JSON bodies; a 404 as a response; refusals (off the list, wrong host for a secret, an engine-only secret) never sent;
  - **a secret can't leak**: through `str`, a fragment, `json()`, `data.set`, the session, `print`, a mail template, or an error message; and isn't in the response, the terminal, `private/state/` (the engine log included) or `data/`;
  - the outbound report and `.json`, and the engine log lines;
  - webhooks: a good signature accepted without cookies or token, even with a foreign `Origin`; a bad, missing or mismatched one refused; GET refused; `post.x` still needs its token; `rawBody` only in webhooks;
  - app mail to a user by id and to an allowed address; refused to others, with a line break in the subject, or to an unknown user; a blind copy to the owner (a repeat of the recipient dropped), a blind copy to a stranger refused, and `cc` refused, with nothing sent; each recipient counted against `appPerHour`;
  - **the weather sample** against a stand-in API: the forecast, untrusted text escaped, a response pointing at the metadata address refused and explained, the contact form's mail, and the signed webhook.

All 22 suites pass with the `dev` and `asan` presets, with `FOURFOLD_TEST_PG_URL` set.

## Still to do

- Jobs, the schedule and SSE push (M9.1); sending mail from a job instead of waiting for it in a request.
- A request inspector showing each request's outbound calls (M12); only the latest calls, app-wide, so far.
- SHA-1 for `hmac.verify`; a way for a webhook route to take more than the default anonymous rate limit (it can only be tightened now).
- Outbound limits in the server config's ceilings (only the app's config can tighten them now); the outbound rate counted somewhere that survives restarts.
- `http.post` with a form-encoded or multipart body (JSON, text and bytes only).
- CGI mode (M10): outbound requests should work as they are, within each request's process.

## Still to check by hand

- **`examples/weather` against the real API, in a browser**: checked from the command line on 2026-11-12 (four periods; both requests in the outbound report).
- **The sample's webhook** with the `curl` command in `code/pages/hooks.ms`.
- **App mail through real SMTP or Postmark**: `config.mail` with a real transport, then the contact form; and a blind copy, to see it arrives without showing in the recipient's headers.
- **The corner menu's new "outbound" link**, and the report's layout.

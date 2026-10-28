# Fourfold: M6 Results — Configuration, auth, and access control

*Status: 2026-10-28, at the end of M6 ([milestones.md](milestones.md)). Records what was built for config, sign-in and access rules, the choices the milestone left open, and what's still to check by hand. No MS2 changes.*

## Summary

| Question | Outcome |
|---|---|
| Can declarations be evaluated at compile time, separately from the file's runtime behavior (R10)? | **Yes; R10 retired.** `code/access.ms` and the config files run in a restricted VM (`DataEvaluator`); a page's `access =` is read from its top level during the compile-time discovery run, which has no data, session or files. Only inert values come out. |
| Is every route gated, static files included? | **Yes.** The policy gate now runs for every route kind (pages, handlers, Markdown, static files), not only handlers, before anything is served. |
| Does access resolve per route, at compile time? | **Yes.** Each `RouteEntry` carries its effective `AccessLevel` and the rules that produced it; the request path does a lookup, never pattern matching. |
| Does magic-link sign-in work end to end? | **Yes, against an SMTP server in the test process.** Against a real mail server: to be checked by hand (see below). |
| Does an invalid config fail safe? | **Yes.** Unknown settings are errors; an invalid reload keeps the previous config; `fourfold serve` won't start with an invalid one. |

---

## What was built

- **`partition`:** `ConfigDir` (read-only, `private/config/`) and `ConfigFile`, which only `ConfigDir` makes, so config reaches the evaluator by type, like code (`CodeFile`). `ConfigDir::readServerConfig` reads the server config by host path. `fourfold dev` writes commented `app.ms` and `secrets.ms` templates into a newly created `private/config/`.
- **`script`:** `DataEvaluator` (implemented by `ScriptHost`): a `Run::Kind::Data` run with no services, a 1M-instruction budget, no `import` and no waiting; its one output global must pass `toDatum`. `PageHandlers.access` from discovery. `req.user` (frozen). The `users` module (`get`, `find`, `grant`, `revoke`) over a `UserDirectory` in `RunContext`. `ScriptHost::setBudget` for config reloads.
- **`compiledapp`:** `AccessLevel` (an AND of OR-sets of roles; empty set = `"login"`), with `require` (only tightens, dropping implied requirements), `allows` and `describe`; `RouteEntry::access` and `accessSources`; `isValidRoleName`; `CompiledApp::authLayout`.
- **`compiler`:** `pass_access.cpp`: reads `code/access.ms` (`rules`), checks paths and values, combines them with each page file's `access =`, and warns about rules that cover nothing. `frags/auth/layout.html` compiles as a whole page, like the Markdown layout. Links to `/__auth/` aren't flagged as broken.
- **`config`** (new): `loadConfig` → `AppConfig` with `SiteConfig`, `ScriptBudget`, `SessionConfig`, `AuthConfig`, `MailConfig`, the secrets, and the `Mailer` built from them. Server ceilings (`server.ceilings.budget`, `.session`) lower app values, with a warning. `configStamp` for change detection.
- **`state`:** `AuthStore` (users, roles, sign-in tokens, in `state.db`), with throttles per email and per address group. `SessionStore::signIn` rotates the ID in one transaction. Both stores' configs can change at run time.
- **`mail`** (new): `Mailer` with SMTP (STARTTLS required, or implicit TLS; plain only to localhost) and Postmark over libcurl, and a log mailer for dev. Messages are structured; addresses and subjects are checked, so no header can be injected. Bodies go out as quoted-printable.
- **`policy`:** `checkAccess` beside `checkCsrf`, each only adding a denial; denials say whether the visitor needs to sign in (401) or lacks a role (403).
- **`pipeline`:** Identify (one worker hop: the session's user id, then the user, only when the visitor has a session cookie and the route is restricted or runs code); the gate for every route; sign-in redirects (`HX-Redirect` for HTMX); `Cache-Control: private, no-store` on restricted responses and on any response to a signed-in visitor; `/__auth/` routed, after Identify and a CSRF check, to an `AuthRoutes` implementation; the config pinned per request. The cookie helpers moved to a public header, `pipeline/cookies.h`.
- **`authflow`** (new): the `/__auth/` pages (login, link, logout, fake); see [the user guide](../user/user_guide.md#signing-in-requser).
- **`lifecycle`:** `AppInstance` polls `private/config/` and swaps a valid new config into the engine.
- **`devtools`:** the access report (`/__dev/access`, and `.json`), and a corner menu on dev pages ("sign in as…", "access").
- **`hosts/cli`:** `AppServer` and the CGI spike load the config, open the `AuthStore`, and wire `AuthFlow`; `fourfold serve --server-config FILE`; the host check accepts `config.domain`.
- **The clubhouse** sample (`examples/clubhouse/`): public, members, moderators and admin areas.

## Choices made here

- **The emailed link shows a page; a POST signs in.** Mail scanners and link previewers fetch links; a GET that signed in would let them use up the token, or sign in on the scanner's side. The page carries the token in a form.
- **"Check your email" whatever happens** (unknown address with sign-up closed included), so the form reveals no accounts. Too many links is a 429, which says nothing about the address either.
- **Throttles in `AuthStore`, not M7's `RateStore`:** three links per email address and ten per visitor address group, per hour. Sign-in by email couldn't ship without them (an open form that sends mail is an abuse tool), and the rule from milestones.md is that security features arrive complete.
- **Sign-up is open by default** (`auth.signUp = "open"`): a first sign-in creates the user. `"closed"` allows existing users and `auth.admins` only. There's no `users.add` yet, so with sign-up closed, users come from a period of open sign-up or from `admins`; adding one from code can come later.
- **A new user's name** is the email's local part; there's no way to change it from code yet (`AuthStore::setName` exists, but no `users` function).
- **Signing in carries the anonymous session's map over, but not another user's.** If a signed-in visitor signs in as someone else, the old map isn't handed to the new user.
- **Initial admins get `"admin"` at sign-in,** not when the config loads, so the store holds no user who has never signed in.
- **Config is MiniScript evaluated as data, with unknown names as errors.** A typo in a security setting (say, `signup` for `signUp`) must not quietly leave the default.
- **Invalid config at startup:** `fourfold serve` refuses to start (its defaults might be wider than intended); `fourfold dev` starts with the defaults and reloads once fixed.
- **Reloaded on change:** auth, mail, budgets and session limits. Not reloaded: `domain` as a host name the app answers to (it's added at startup), and the server config (read at startup).
- **Mail needs a domain in serve mode.** Sign-in links point to `https://<domain>`, never to the request's Host header, which an attacker could set when asking for someone else's link. In dev, links use the request's origin (localhost).
- **A secret used by the engine must have no `hosts`.** Using an app-visible secret as the SMTP password is an error, so it can't be both.
- **`budget.memoryMB` is an error, not ignored:** there's no per-request memory limit yet (R4), and a setting that silently did nothing would mislead.
- **Personal responses aren't cached by others:** any response to a signed-in visitor gets `private, no-store`, not just restricted routes, since a public page may show who's asking.
- **The fake login's users** are an admin, a member with no roles, and one per other role named by the app's access rules, each a real `AuthStore` user at `@fake.invalid`. Fake login is a config error in serve mode.
- **The sign-in pages' layout is `frags/auth/layout.html`**, one wrapper with `{{title}}` and `{{content}}`, rather than a template per page.
- **libcurl** (the system's) for SMTP and HTTPS, chosen with the user; it touches only the `mail` component. Server-side TLS (M11) is a separate decision.

## Tests

- `tests/partition/app_folder_test.cpp`: config templates; `ConfigDir` refuses links and folders; stamps.
- `tests/script/script_host_test.cpp`: the evaluator's restrictions (no `data`, `file`, `dropbox`, `users`, `import`, `wait`; budget; non-inert results); `access =` discovery; `req.user`; the `users` module.
- `tests/compiler/routes_test.cpp`: rules over folders, files, Markdown and single handlers on whole segments; combination with `access =`; `AccessLevel` only tightening; bad rules; the unmatched-rule warning; a golden file for the clubhouse.
- `tests/config/app_config_test.cpp`: defaults per mode; every setting; mistakes as errors; secrets; ceilings.
- `tests/state/`: users, roles, tokens (single use, expiry, throttles); session rotation.
- `tests/mail/mailer_test.cpp`: address checks, header injection, message format, SMTP and Postmark against servers in the test, STARTTLS required.
- `tests/engine/policy/policy_gate_test.cpp`: every access level against every kind of visitor and method.
- `tests/engine/authflow/auth_flow_test.cpp`: `next` stays on the site; fake users.
- `tests/hosts/cli/access_test.cpp`, end to end:
  - **the exit criterion:** every route of the clubhouse (pages, handlers, Markdown, an image) requested as an anonymous visitor, a member, a moderator and an admin; the results match the access report;
  - magic-link sign-in through an SMTP server in the test: rotation, the cart carried over, the old ID dead, a link used twice, a configured admin, sign out, sign out everywhere, the throttle, a forged sign-out, and a config reload closing sign-up;
  - `fourfold serve` refusing fake login and a missing host name.

All 21 suites pass with the `dev`, `ci` and `asan` presets.

## Still to check by hand

**Sign-in against a real mail server** (the last exit criterion). The tests can't reach one from the sandbox they run in. (Setting mail up is covered in the [admin guide](../admin/admin_guide.md#mail-for-sign-in).) To check:

1. In `examples/clubhouse/private/config/secrets.ms`: `secrets = {"smtpPassword": {"value": "<password>"}}` (or a Postmark server token).
2. In `app.ms`, uncomment and fill in `config.mail` (SMTP or Postmark), with a `from` address the server may send as.
3. `./build/dev/bin/fourfold dev examples/clubhouse`, open http://localhost:8090/__auth/login, and sign in with a real address. The mail should arrive with a working link; a failure is printed in the terminal and the engine log.

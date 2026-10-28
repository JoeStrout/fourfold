# Clubhouse (M6 sample)

Sign-in and access rules: a public area, a members area (pages, a Markdown
page and an image), a moderators' page, and an admin area where admins make
other users moderators.

```sh
./build/dev/bin/fourfold dev examples/clubhouse
```

Then open http://localhost:8090/.

- **Signing in.** In dev mode, the sign-in page (and the corner menu on every
  page) offers "sign in as..." test users: an admin, a member with no roles,
  and a moderator (because `code/access.ms` names that role). Or sign in by
  email: the link is printed in the terminal. `admin@example.com` becomes an
  admin when it signs in (`private/config/app.ms`).
- **The rules** are in `code/access.ms`: `/members` needs sign-in, `/mod` needs
  the admin or moderator role, `/admin` needs admin. `pages/admin/index.ms`
  says `access = "admin"` as well. `/board` is public, but posting to it
  (`/board/post`) needs sign-in, so the board's `post.post` can use
  `req.user.name` without checking.
- **Visitors who aren't allowed** are sent to sign in (a page), or get 401
  (a post) or 403 (signed in, without the role). The handlers never run.
- **The access report**, at http://localhost:8090/__dev/access, lists every
  URL with its access level and the rules that produced it.
- **Roles change at once:** make a member a moderator on `/admin/`, and their
  next request to `/mod/` is let in.
- **Real email:** set `config.mail` in `private/config/app.ms` (SMTP or
  Postmark) and put the password or token in `private/config/secrets.ms`.
  The config is read again when it changes.

| File | Shows |
|---|---|
| `code/access.ms` | rules for folders, a single handler, and a list of roles |
| `pages/admin/index.ms` | `access =`, and `users.find`, `users.grant`, `users.revoke` |
| `pages/board.ms` | a public page whose post handler needs sign-in; `req.user` |
| `lib/nav.ms` | showing who's signed in, or a sign-in link with `next` |
| `frags/auth/layout.html` | the sign-in pages in the site's own design |
| `frags/nav/in.html` | signing out: a form posting to `/__auth/logout` |
| `private/config/app.ms` | the first admin, and where mail settings go |

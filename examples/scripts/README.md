# Scripts (M2 and M3 sample)

Pages with MiniScript: `render` filling a page's fields, `get` and `post`
handlers, a module in `lib/`, redirects, JSON, and what happens when code goes
wrong.

```sh
./build/dev/bin/fourfold dev examples/scripts
```

Then open http://localhost:8090/, and try editing files while it runs (M3):

- Change `pages/hello.html` or `hello.ms` and save: the page reloads by itself.
- Change `pages/style.css`: the stylesheet is swapped in place, without a reload.
- Break `hello.ms` (delete an `end function`): open pages show the error, with
  the code around it, while the site keeps serving the last good version. Fix
  it, and they reload.

| File | Shows |
|---|---|
| `pages/hello.html`, `hello.ms` | `render` filling `{{fields}}`; `import "words"` from `lib/` |
| `pages/contact.ms` | `post.send` at `/contact/send`, `status(400, ...)`, `redirect(...)`, `print` |
| `pages/api.ms` | `get.echo` at `/api/echo`, returning `json(...)` |
| `pages/slow.ms` | a page made only of code; `wait` parks the request, not the server |
| `pages/oops.ms` | a runtime error: dev mode shows the file and line |
| `pages/forever.ms` | an endless loop, ended by the instruction budget |

# Scripts (M2 sample)

Pages with MiniScript: `render` filling a page's fields, `get` and `post`
handlers, a module in `lib/`, redirects, JSON, and what happens when code goes
wrong.

```sh
./build/dev/bin/fourfold dev examples/scripts
```

Then open http://localhost:8080/.

| File | Shows |
|---|---|
| `pages/hello.html`, `hello.ms` | `render` filling `{{fields}}`; `import "words"` from `lib/` |
| `pages/contact.ms` | `post.send` at `/contact/send`, `status(400, ...)`, `redirect(...)`, `print` |
| `pages/api.ms` | `get.echo` at `/api/echo`, returning `json(...)` |
| `pages/slow.ms` | a page made only of code; `wait` parks the request, not the server |
| `pages/oops.ms` | a runtime error: dev mode shows the file and line |
| `pages/forever.ms` | an endless loop, ended by the instruction budget |

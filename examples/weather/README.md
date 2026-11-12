# Weather (M9 sample)

Outbound HTTP, a webhook and app mail: a forecast page that calls the
National Weather Service's API, a contact form that mails the site owner,
and a webhook that checks its caller's signature.

```sh
./build/dev/bin/fourfold dev examples/weather
```

Then open http://localhost:8090/, and http://localhost:8090/__dev/outbound.

- `code/outbound.ms` lists the one host the app's code may reach,
  `api.weather.gov`, and the one address besides users it may mail. Take
  the host out and the page says why it can't get a forecast; the request
  is never sent.
- The forecast is two requests: the second goes to a URL the first one
  returned. That URL is checked like any other, so a response can't steer
  the app somewhere else (and nor could a visitor, if the page took a URL).
- Every request is checked as it connects: private and internal addresses
  (127.0.0.1, 10.x, 169.254.169.254 where cloud credentials live, and the
  rest) are always refused, even if a name on the list points there.
- Each request is in the outbound report, and in the engine log
  (`private/state/logs/`), with the query string left out.
- The contact form (`/contact`) sends its message with `mail.send` to
  `owner@example.com`, the address in `mailTo`, from the template
  `code/mail/contact.md`. In dev mode the message is printed in the terminal.
- `/hooks/deploy` is a webhook (`webhook.deploy` in `code/pages/hooks.ms`):
  it takes posts with no CSRF token, and checks an HMAC signature instead,
  with the secret `hookKey` from `private/config/secrets.ms`. That secret
  has `"verify": true`, so code can check signatures with it but never see
  or send it. The comment in `hooks.ms` has a `curl` command to try it.
- A secret meant for an API looks like
  `secrets.weatherKey = {"value": "...", "hosts": ["api.example.com"]}`, and
  is used as `{"Authorization": ["Bearer ", secrets.weatherKey]}`. App code
  holds only a handle: printing it, putting it in a page, `data` or the
  session, or sending it to any other host, all fail.

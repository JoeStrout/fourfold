# Newsfeed (M9.1 sample)

Background jobs and push: a scheduled job polls Hacker News's top stories,
and each new one appears on open pages as it's found, over server-sent
events, without a reload. A second job mails the owner a digest every
morning.

```sh
./build/dev/bin/fourfold dev examples/newsfeed
```

Then open http://localhost:8090/, and the jobs panel at
http://localhost:8090/__dev/jobs. It needs an internet connection (the
stories come from `hacker-news.firebaseio.com`).

- `code/jobs/stories.ms` defines `job.pollStories`, and
  `code/schedule.ms` runs it `"every 5m"`. Its first run is when the server
  starts. It fetches the top ten story ids, then each new story, keeps the
  newest 30 in `data/stories.json`, and calls `publish "stories", frag(...)`
  for each new one.
- The page subscribes with
  `<ol hx-ext="sse" sse-connect="/live/stories" sse-swap="message" hx-swap="afterbegin">`.
  HTMX's SSE extension comes with `/__fourfold/htmx.js`. Each published
  fragment is added at the top of the list.
- `/live/stories` is served because `code/access.ms` has a rule for it
  (`"public"`). A channel no rule covers isn't served, and a rule like
  `"login"` makes it members-only, like a page.
- `/poll` is the same page for hosting without live updates (CGI, on shared
  hosting; see the admin guide's "Shared hosting"): it fetches the list from
  `get.stories` every 30 seconds (`hx-trigger="load, every 30s"`), and the
  jobs run when cron runs `fourfold run-jobs`.
- "Check now" posts to `post.refresh`, which runs
  `enqueue "pollStories", null, {"key": "refresh"}`. The key makes the job
  unique while it waits, so clicking again adds nothing. An enqueued job is
  committed only if the request succeeds.
- `job.dailyDigest` runs `"daily 07:00"` in `America/Denver`
  (`config.jobs.timeZone` in `private/config/app.ms`). It mails the top five
  stories to `owner@example.com` (`mailTo` in `code/outbound.ms`), using the
  Markdown template `code/mail/digest.md`. In dev mode the mail is printed in
  the terminal. Use "Run now" in the jobs panel to try it without waiting.
- A job that fails (no network, say) returns an error value, and is tried
  again later (15 s, then 30 s, 1 min, ...). After five attempts it's dead,
  and the jobs panel shows it with Retry and Discard buttons. "Pause the
  schedule" stops the schedule from firing while you look.

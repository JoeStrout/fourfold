// Mails the site owner the day's top stories, each morning at 07:00 in the
// app's time zone (code/schedule.ms).

job.dailyDigest = function(args)
    stories = data.get("stories", [])
    if not stories then return null
    top = stories[:]
    top.sort "score", false
    return mail.send("owner@example.com", "Today's top stories", "digest", {"stories": frag("digest-item", top[:5])})
end function

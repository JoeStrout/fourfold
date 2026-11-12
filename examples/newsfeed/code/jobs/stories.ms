// Polls Hacker News's top stories (https://github.com/HackerNews/API), keeps
// the newest in data/stories.json, and sends each new one to the pages open
// on /live/stories.

api = "https://hacker-news.firebaseio.com/v0/"

// A story as the page shows it, from the API's item; null if it isn't one.
storyFrom = function(id, item)
    if item == null or not item.hasIndex("title") then return null
    url = "https://news.ycombinator.com/item?id=" + id
    if item.hasIndex("url") then url = item.url
    score = 0
    if item.hasIndex("score") then score = item.score
    by = ""
    if item.hasIndex("by") then by = item.by
    return {"id": id, "title": item.title, "url": url, "score": score, "by": by}
end function

job.pollStories = function(args)
    top = http.get(api + "topstories.json")
    if top isa error then return top    // an error value: tried again later
    if top.status != 200 then return err("topstories.json answered " + top.status)
    stories = data.get("stories", [])
    seen = {}
    for s in stories
        seen[s.id] = true
    end for
    fresh = []
    for id in top.json[:10]
        if seen.hasIndex(id) then continue
        resp = http.get(api + "item/" + id + ".json")
        if resp isa error or resp.status != 200 then continue
        story = storyFrom(id, resp.json)
        if story != null then fresh.push story
    end for
    if not fresh then return null
    // Oldest first, so each lands at the top of the list in turn.
    for i in range(fresh.len - 1, 0)
        stories.insert 0, fresh[i]
        publish "stories", frag("story", fresh[i])
    end for
    data.set "stories", stories[:30]
end function

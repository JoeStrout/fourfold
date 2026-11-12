render = function(req)
    return {"stories": frag("story", data.get("stories", []))}
end function

// "Check now": queues the poll at once. With the key, clicking again while
// it's waiting adds nothing.
post.refresh = function(req)
    enqueue "pollStories", null, {"key": "refresh"}
    return redirect("/")
end function

// The list alone, for /poll: where live updates aren't available (an app
// served through CGI, on shared hosting), the page asks for it instead.
get.stories = function(req)
    return frag("story", data.get("stories", []))
end function

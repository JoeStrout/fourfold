render = function(req)
    return {"stories": frag("story", data.get("stories", []))}
end function

// "Check now": queues the poll at once. With the key, clicking again while
// it's waiting adds nothing.
post.refresh = function(req)
    enqueue "pollStories", null, {"key": "refresh"}
    return redirect("/")
end function

get.entries = function(req)
    return json(["Ada", "Grace"])
end function

post.entries = function(req)
    return status(405, "use /guestbook/sign")
end function

post.sign = function(req)
    return redirect("/guestbook")
end function

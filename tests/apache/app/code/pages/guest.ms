form.sign = {"name": {"type": "text", "required": true, "max": 40}}

post.sign = function(req)
    data.set "lastGuest", req.form.name
    return "signed by " + req.form.name
end function

get.last = function(req)
    return data.get("lastGuest", "nobody")
end function

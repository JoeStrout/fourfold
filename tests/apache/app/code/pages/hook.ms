// A webhook: no CSRF; shows the Authorization header Apache passed along.
webhook.ping = function(req)
    if req.headers.hasIndex("authorization") then return "auth: " + req.headers.authorization
    return "auth: none"
end function

// A counter in the session: proves sessions carry across CGI processes.
get.up = function(req)
    if not session.hasIndex("n") then session.n = 0
    session.n = session.n + 1
    return str(session.n)
end function

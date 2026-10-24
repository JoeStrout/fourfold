// A page made only of code (there's no slow.html): render returns text.
// `wait` parks this request's VM; other requests keep being served meanwhile.
render = function(req)
    wait 1
    return "That took a second, but nobody else had to wait."
end function

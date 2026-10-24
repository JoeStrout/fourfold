// Fills hello.html's {{greeting}} and {{letters}}.
import "words"

render = function(req)
    name = "stranger"
    if req.query.hasIndex("name") and req.query.name != "" then name = req.query.name
    return {
    	"greeting": words.greet(name), 
    	"letters": name.len,
    }
end function

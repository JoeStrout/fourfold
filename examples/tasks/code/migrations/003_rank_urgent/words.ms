// Copied here, not imported from code/lib/: a migration can't depend on code
// that may change after it's applied.
soundsUrgent = function(title)
    t = title.lower
    for w in ["urgent", "asap", "today"]
        if t.indexOf(w) != null then return true
    end for
    return false
end function

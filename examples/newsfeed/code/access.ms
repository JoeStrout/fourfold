// Push channels are paths under /live/, and need a rule like any page:
// a channel no rule covers isn't served.
rules = {}
rules["/live/stories"] = "public"

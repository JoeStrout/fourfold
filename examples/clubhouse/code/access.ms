// Who may use what (overview §5, Access control). Each rule covers its path
// and everything below it: "/members" covers /members/handbook and
// /members/badge.svg too. Every rule that applies must be met.
rules = {}
rules["/members"] = "login"
rules["/mod"] = ["admin", "moderator"]
rules["/admin"] = "admin"
// The board is public to read; posting to it needs an account.
rules["/board/post"] = "login"

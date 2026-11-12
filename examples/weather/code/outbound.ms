// Where this app's code may send requests and mail (overview §6). Without
// this file, http can't reach anything, and mail goes only to users.
allow = ["api.weather.gov"]

// Besides users (by id), the one address mail.send may write to: the site
// owner, for the contact form.
mailTo = ["owner@example.com"]

// Spam rules (overview §5, Rate limits and spam), read at compile time.
// A rule covers its path and everything below it.
limits = {}

// Signing: at most 5 a minute and 50 a day from one address, and not
// sooner than 2 seconds after the form was shown (bots post at once).
limits["/sign"] = {"rate": ["5/minute", "50/day"], "minSeconds": 2}

// Uploading a photo: 10 an hour, and a proof-of-work puzzle the page's
// JavaScript solves while the visitor picks a file.
limits["/photos/upload"] = {"rate": "10/hour", "challenge": true}

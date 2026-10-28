// The Clubhouse's config (overview §10): MiniScript, evaluated as data.
config = {}

// These addresses get the admin role when they sign in.
config.auth = {"admins": ["admin@example.com"]}

// To send real sign-in emails (instead of printing them, as dev mode does),
// set up SMTP or Postmark, with the password or token in secrets.ms:
//
// config.mail = {"transport": "smtp", "from": "Clubhouse <noreply@example.com>",
//     "smtpHost": "smtp.example.com", "smtpUser": "noreply@example.com",
//     "passwordSecret": "smtpPassword"}
//
// config.mail = {"transport": "postmark", "from": "Clubhouse <noreply@example.com>",
//     "postmarkTokenSecret": "postmarkToken"}

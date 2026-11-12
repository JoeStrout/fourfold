// Secrets (overview §10). Never commit real ones.
//
// A secret with "hosts" can be sent by app code to those hosts only; one
// with "verify": true can check signatures (hmac.verify) and is never sent;
// one with neither is for the engine only (SMTP or database passwords).
secrets = {}
secrets.hookKey = {"value": "dev-only-hook-key", "verify": true}

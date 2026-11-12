// A webhook: another server posts here, with no CSRF token (it isn't a
// browser), and signs what it sends with a key you share with it. Try:
//
//   body='{"event":"deploy"}'
//   sig=$(printf '%s' "$body" | openssl dgst -sha256 -hmac 'dev-only-hook-key' | sed 's/.* //')
//   curl -X POST http://localhost:8090/hooks/deploy -H "Content-Type: application/json" \
//        -H "X-Signature: sha256=$sig" -d "$body"
webhook.deploy = function(req)
    sig = ""
    if req.headers.hasIndex("x-signature") then sig = req.headers["x-signature"]
    if not hmac.verify("sha256", secrets.hookKey, req.rawBody, sig) then return status(401, "bad signature")
    data.append "deploys", {"event": req.json.event}
    return "ok"
end function

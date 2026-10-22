#include "pipeline/stages.h"

namespace fourfold {

namespace {

// Overview §8, "Content Security Policy", without the script nonce for now.
const char* const kContentSecurityPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
    "img-src 'self' https: data:; connect-src 'self'; object-src 'none'; "
    "base-uri 'none'; form-action 'self'; frame-ancestors 'none'";

const char* const kPermissionsPolicy =
    "camera=(), microphone=(), geolocation=(), payment=(), usb=(), "
    "serial=(), bluetooth=(), display-capture=()";

}  // namespace

void addStandardHeaders(HttpResponse& response) {
    Headers& h = response.headers;
    h.set("Content-Security-Policy", kContentSecurityPolicy);
    h.set("X-Frame-Options", "DENY");
    h.set("X-Content-Type-Options", "nosniff");
    h.set("Referrer-Policy", "strict-origin-when-cross-origin");
    h.set("Cross-Origin-Opener-Policy", "same-origin");
    h.set("Permissions-Policy", kPermissionsPolicy);
    // Dev mode: always revalidate, so edits show on reload. (Production
    // caching rules come with ETags, later.)
    if (!h.has("Cache-Control")) h.set("Cache-Control", "no-cache");
}

}  // namespace fourfold

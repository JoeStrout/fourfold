// The engine's cookies (overview §5, Sessions and cookies), and the
// visitor's address group. Shared by the pipeline and `authflow`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "http/message.h"

namespace fourfold {

// The value of cookie `name` in the request's Cookie headers, if any.
std::optional<std::string> cookieValue(const HttpRequest& request, const std::string& name);

// A cookie's name: "__Host-ff_<base>" over HTTPS, which the browser then
// keeps to this host and to HTTPS; "ff_<base>" otherwise (dev mode).
std::string cookieName(const std::string& base, bool https);

// A Set-Cookie value: Path=/, HttpOnly, SameSite=Lax, and Secure over HTTPS.
std::string setCookie(const std::string& base, const std::string& value, bool https, int64_t maxAgeSeconds);

// The visitor's address group, for the anonymous-session caps: an IPv4
// address as is, an IPv6 address's /64 (overview §5, Rate limits: IPv6
// addresses are handed out by the /64).
std::string addressGroup(const std::string& peerAddress);

}  // namespace fourfold

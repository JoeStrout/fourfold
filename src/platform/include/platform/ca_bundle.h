// The certificate authorities' file for TLS clients (libcurl: outbound HTTP
// and mail), when the one the TLS library was built to read isn't there.
//
// A static Linux build (tools/linux/build-static.sh, for shared hosting)
// carries the path its build system used (Alpine's /etc/ssl/cert.pem), but
// it runs on whatever Linux the host has, which may keep the file elsewhere
// (Debian and Ubuntu: /etc/ssl/certs/ca-certificates.crt). Without this,
// every HTTPS request would fail to verify.
#pragma once

#include <optional>
#include <string>

namespace fourfold {

// The file to use instead of `builtIn` (the library's own default, or null
// if it has none, as with a TLS library that uses the system's store):
// nullopt if `builtIn` is null or readable; otherwise $SSL_CERT_FILE if set
// and readable, else the first readable file of the usual places (Debian,
// Ubuntu, Alpine, Red Hat, SUSE), else nullopt. Found once per `builtIn`.
std::optional<std::string> caBundleFallback(const char* builtIn);

}  // namespace fourfold

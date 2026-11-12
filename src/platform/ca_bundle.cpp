#include "platform/ca_bundle.h"

#include <cstdlib>
#include <mutex>
#include <unistd.h>

namespace fourfold {

namespace {

bool readable(const char* path) { return path && *path && ::access(path, R_OK) == 0; }

std::optional<std::string> find(const char* builtIn) {
    if (!builtIn || readable(builtIn)) return std::nullopt;
    if (const char* env = std::getenv("SSL_CERT_FILE"); readable(env)) return std::string(env);
    static const char* const kPlaces[] = {
        "/etc/ssl/certs/ca-certificates.crt",                  // Debian, Ubuntu, Alpine, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",                    // Red Hat, CentOS, Fedora
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",   // newer Red Hat
        "/etc/ssl/cert.pem",                                   // Alpine, the BSDs, macOS
        "/etc/ssl/ca-bundle.pem",                              // SUSE
    };
    for (const char* p : kPlaces) {
        if (readable(p)) return std::string(p);
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> caBundleFallback(const char* builtIn) {
    static std::mutex mutex;
    static std::string forBuiltIn;
    static std::optional<std::string> found;
    static bool done = false;
    std::lock_guard<std::mutex> lock(mutex);
    std::string key = builtIn ? builtIn : "";
    if (!done || key != forBuiltIn) {
        found = find(builtIn);
        forBuiltIn = key;
        done = true;
    }
    return found;
}

}  // namespace fourfold

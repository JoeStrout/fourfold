// Keys and certificates, for the server's TLS and for ACME (overview §8,
// TLS). PEM text in and out, so callers never touch OpenSSL's types.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "base/result.h"

namespace fourfold {

// A new private key: ECDSA on P-256, as PKCS#8 PEM.
Result<std::string> generatePrivateKey();

struct CertificateInfo {
    std::vector<std::string> names;   // DNS names (subjectAltName, else the CN), lowercased
    int64_t notBefore = 0;            // Unix seconds
    int64_t notAfter = 0;
    std::string issuer;               // one line, e.g. "CN=R10,O=Let's Encrypt,C=US"
};

// The first certificate of a PEM chain.
Result<CertificateInfo> inspectCertificate(const std::string& chainPem);

// Whether `keyPem` is the private key of the chain's first certificate.
Status checkKeyMatches(const std::string& chainPem, const std::string& keyPem);

// A self-signed certificate for `names`, valid from an hour ago for `days`
// days. For tests, and for serving TLS before a real certificate arrives.
Result<std::string> selfSignedCertificate(const std::vector<std::string>& names, const std::string& keyPem,
                                          int days);

// A certificate signing request for `names` (the first is also the CN),
// signed with `keyPem`, as DER (what ACME's finalize takes, base64url'd).
Result<std::string> certificateRequestDer(const std::vector<std::string>& names, const std::string& keyPem);

// JWS for ACME (RFC 7515, 7518, 7638), with a P-256 key from
// generatePrivateKey():
//
// The public key as a JWK, in the canonical form its thumbprint hashes:
// {"crv":"P-256","kty":"EC","x":"…","y":"…"}.
Result<std::string> publicJwk(const std::string& keyPem);
// An ES256 signature of `data`: r and s, 32 bytes each (not DER).
Result<std::string> signEs256(const std::string& keyPem, const std::string& data);

// Whether `host` matches `pattern`: equal ignoring case, or a wildcard
// "*.example.com" matching exactly one label ("a.example.com").
bool hostMatches(const std::string& pattern, const std::string& host);

}  // namespace fourfold

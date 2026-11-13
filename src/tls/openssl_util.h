// OpenSSL helpers shared within the tls component (private).
#pragma once

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <memory>
#include <string>

#include "base/result.h"

namespace fourfold {

struct BioFree {
    void operator()(BIO* b) const { BIO_free(b); }
};
struct KeyFree {
    void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
};
struct X509Free {
    void operator()(X509* x) const { X509_free(x); }
};
struct BnFree {
    void operator()(BIGNUM* b) const { BN_free(b); }
};
using UniqueBio = std::unique_ptr<BIO, BioFree>;
using UniqueKey = std::unique_ptr<EVP_PKEY, KeyFree>;
using UniqueX509 = std::unique_ptr<X509, X509Free>;
using UniqueBn = std::unique_ptr<BIGNUM, BnFree>;

// `what`, followed by OpenSSL's queued errors (which this clears).
std::string openSslError(const std::string& what);
Error openSslFailure(ErrorCode code, const std::string& what);

// Null if the PEM can't be read.
UniqueKey readPrivateKey(const std::string& pem);
UniqueX509 readCertificate(const std::string& pem);
// A memory BIO's contents.
std::string bioText(BIO* bio);

}  // namespace fourfold

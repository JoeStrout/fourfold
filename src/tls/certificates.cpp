#include "tls/certificates.h"

#include <openssl/asn1.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cctype>
#include <ctime>
#include <memory>

#include "base/crypto.h"
#include "openssl_util.h"

namespace fourfold {

std::string openSslError(const std::string& what) {
    std::string message = what;
    unsigned long code;
    bool first = true;
    while ((code = ERR_get_error()) != 0) {
        char buffer[256];
        ERR_error_string_n(code, buffer, sizeof buffer);
        message += first ? ": " : "; ";
        message += buffer;
        first = false;
    }
    return message;
}

Error openSslFailure(ErrorCode code, const std::string& what) {
    return makeError(code, openSslError(what));
}

UniqueKey readPrivateKey(const std::string& pem) {
    UniqueBio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) return nullptr;
    return UniqueKey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

UniqueX509 readCertificate(const std::string& pem) {
    UniqueBio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) return nullptr;
    return UniqueX509(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

std::string bioText(BIO* bio) {
    char* data = nullptr;
    long size = BIO_get_mem_data(bio, &data);
    return size > 0 ? std::string(data, static_cast<size_t>(size)) : std::string();
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int64_t asn1Seconds(const ASN1_TIME* t) {
    struct tm tm {};
    if (!t || ASN1_TIME_to_tm(t, &tm) != 1) return 0;
    return static_cast<int64_t>(timegm(&tm));
}

// The certificate's DNS names: its subjectAltName's, else its CN.
std::vector<std::string> certificateNames(X509* cert) {
    std::vector<std::string> names;
    auto* sans = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (sans) {
        for (int i = 0; i < sk_GENERAL_NAME_num(sans); i++) {
            const GENERAL_NAME* n = sk_GENERAL_NAME_value(sans, i);
            if (n->type != GEN_DNS) continue;
            const unsigned char* data = ASN1_STRING_get0_data(n->d.dNSName);
            int length = ASN1_STRING_length(n->d.dNSName);
            names.push_back(lower(std::string(reinterpret_cast<const char*>(data), static_cast<size_t>(length))));
        }
        GENERAL_NAMES_free(sans);
    }
    if (names.empty()) {
        char cn[256];
        if (X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof cn) > 0) {
            names.push_back(lower(cn));
        }
    }
    return names;
}

std::string oneLine(const X509_NAME* name) {
    UniqueBio bio(BIO_new(BIO_s_mem()));
    if (!bio) return "";
    X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253);
    return bioText(bio.get());
}

// A subjectAltName extension value listing `names`: "DNS:a,DNS:b".
std::string altNames(const std::vector<std::string>& names) {
    std::string s;
    for (const std::string& n : names) s += (s.empty() ? "DNS:" : ",DNS:") + n;
    return s;
}

bool validNames(const std::vector<std::string>& names) {
    if (names.empty()) return false;
    for (const std::string& n : names) {
        // Commas would split the extension value; nothing else needs care.
        if (n.empty() || n.size() > 253 || n.find_first_of(",: \t\r\n") != std::string::npos) return false;
    }
    return true;
}

}  // namespace

Result<std::string> generatePrivateKey() {
    UniqueKey key(EVP_EC_gen("P-256"));
    if (!key) return openSslFailure(ErrorCode::Internal, "can't generate a key");
    UniqueBio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        return openSslFailure(ErrorCode::Internal, "can't write a key");
    }
    return bioText(bio.get());
}

Result<CertificateInfo> inspectCertificate(const std::string& chainPem) {
    UniqueX509 cert = readCertificate(chainPem);
    if (!cert) return openSslFailure(ErrorCode::Invalid, "not a PEM certificate");
    CertificateInfo info;
    info.names = certificateNames(cert.get());
    info.notBefore = asn1Seconds(X509_get0_notBefore(cert.get()));
    info.notAfter = asn1Seconds(X509_get0_notAfter(cert.get()));
    info.issuer = oneLine(X509_get_issuer_name(cert.get()));
    return info;
}

Status checkKeyMatches(const std::string& chainPem, const std::string& keyPem) {
    UniqueX509 cert = readCertificate(chainPem);
    if (!cert) return openSslFailure(ErrorCode::Invalid, "not a PEM certificate");
    UniqueKey key = readPrivateKey(keyPem);
    if (!key) return openSslFailure(ErrorCode::Invalid, "not a PEM private key");
    if (X509_check_private_key(cert.get(), key.get()) != 1) {
        ERR_clear_error();
        return makeError(ErrorCode::Invalid, "the private key isn't the certificate's");
    }
    return {};
}

Result<std::string> selfSignedCertificate(const std::vector<std::string>& names, const std::string& keyPem,
                                          int days) {
    if (!validNames(names)) return makeError(ErrorCode::Invalid, "invalid host names for a certificate");
    UniqueKey key = readPrivateKey(keyPem);
    if (!key) return openSslFailure(ErrorCode::Invalid, "not a PEM private key");
    UniqueX509 cert(X509_new());
    if (!cert) return openSslFailure(ErrorCode::Internal, "can't make a certificate");
    X509_set_version(cert.get(), 2);
    unsigned char serial[16];
    if (RAND_bytes(serial, sizeof serial) != 1) return openSslFailure(ErrorCode::Internal, "no randomness");
    serial[0] &= 0x7f;   // positive
    UniqueBn bn(BN_bin2bn(serial, sizeof serial, nullptr));
    BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(cert.get()));
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(days) * 86400);
    X509_NAME* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(names[0].c_str()),
                               -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);
    X509_set_pubkey(cert.get(), key.get());
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert.get(), cert.get(), nullptr, nullptr, 0);
    std::string san = altNames(names);
    for (auto [nid, value] : {std::pair<int, const char*>{NID_subject_alt_name, san.c_str()},
                              {NID_basic_constraints, "critical,CA:FALSE"},
                              {NID_ext_key_usage, "serverAuth"}}) {
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
        if (!ext) return openSslFailure(ErrorCode::Internal, "can't make a certificate extension");
        X509_add_ext(cert.get(), ext, -1);
        X509_EXTENSION_free(ext);
    }
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) {
        return openSslFailure(ErrorCode::Internal, "can't sign a certificate");
    }
    UniqueBio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_X509(bio.get(), cert.get()) != 1) {
        return openSslFailure(ErrorCode::Internal, "can't write a certificate");
    }
    return bioText(bio.get());
}

Result<std::string> certificateRequestDer(const std::vector<std::string>& names, const std::string& keyPem) {
    if (!validNames(names)) return makeError(ErrorCode::Invalid, "invalid host names for a certificate");
    UniqueKey key = readPrivateKey(keyPem);
    if (!key) return openSslFailure(ErrorCode::Invalid, "not a PEM private key");
    std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)> req(X509_REQ_new(), X509_REQ_free);
    if (!req) return openSslFailure(ErrorCode::Internal, "can't make a certificate request");
    X509_REQ_set_version(req.get(), 0);
    X509_NAME* subject = X509_REQ_get_subject_name(req.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(names[0].c_str()),
                               -1, -1, 0);
    X509_REQ_set_pubkey(req.get(), key.get());
    STACK_OF(X509_EXTENSION)* exts = sk_X509_EXTENSION_new_null();
    std::string san = altNames(names);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str());
    if (!ext) {
        sk_X509_EXTENSION_free(exts);
        return openSslFailure(ErrorCode::Internal, "can't make a certificate request");
    }
    sk_X509_EXTENSION_push(exts, ext);
    X509_REQ_add_extensions(req.get(), exts);
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    if (X509_REQ_sign(req.get(), key.get(), EVP_sha256()) <= 0) {
        return openSslFailure(ErrorCode::Internal, "can't sign a certificate request");
    }
    int size = i2d_X509_REQ(req.get(), nullptr);
    if (size <= 0) return openSslFailure(ErrorCode::Internal, "can't encode a certificate request");
    std::string der(static_cast<size_t>(size), '\0');
    auto* p = reinterpret_cast<unsigned char*>(&der[0]);
    i2d_X509_REQ(req.get(), &p);
    return der;
}

Result<std::string> publicJwk(const std::string& keyPem) {
    UniqueKey key = readPrivateKey(keyPem);
    if (!key) return openSslFailure(ErrorCode::Invalid, "not a PEM private key");
    if (EVP_PKEY_get_base_id(key.get()) != EVP_PKEY_EC) return makeError(ErrorCode::Invalid, "not an EC key");
    char curve[64] = {0};
    size_t curveLength = 0;
    if (EVP_PKEY_get_utf8_string_param(key.get(), "group", curve, sizeof curve, &curveLength) != 1 ||
        std::string(curve) != "prime256v1") {
        ERR_clear_error();
        return makeError(ErrorCode::Invalid, "not a P-256 key");
    }
    BIGNUM* x = nullptr;
    BIGNUM* y = nullptr;
    if (EVP_PKEY_get_bn_param(key.get(), "qx", &x) != 1 || EVP_PKEY_get_bn_param(key.get(), "qy", &y) != 1) {
        BN_free(x);
        BN_free(y);
        return openSslFailure(ErrorCode::Internal, "can't read the public key");
    }
    UniqueBn bx(x), by(y);
    unsigned char xb[32], yb[32];
    if (BN_bn2binpad(x, xb, 32) != 32 || BN_bn2binpad(y, yb, 32) != 32) {
        return openSslFailure(ErrorCode::Internal, "can't read the public key");
    }
    return std::string("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"") +
           toBase64Url(std::string_view(reinterpret_cast<const char*>(xb), 32)) + "\",\"y\":\"" +
           toBase64Url(std::string_view(reinterpret_cast<const char*>(yb), 32)) + "\"}";
}

Result<std::string> signEs256(const std::string& keyPem, const std::string& data) {
    UniqueKey key = readPrivateKey(keyPem);
    if (!key) return openSslFailure(ErrorCode::Invalid, "not a PEM private key");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    size_t length = 0;
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestSign(ctx.get(), nullptr, &length, reinterpret_cast<const unsigned char*>(data.data()),
                       data.size()) != 1) {
        return openSslFailure(ErrorCode::Internal, "can't sign");
    }
    std::string der(length, '\0');
    if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(&der[0]), &length,
                       reinterpret_cast<const unsigned char*>(data.data()), data.size()) != 1) {
        return openSslFailure(ErrorCode::Internal, "can't sign");
    }
    // DER (SEQUENCE of two INTEGERs) to r || s.
    const auto* p = reinterpret_cast<const unsigned char*>(der.data());
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(length)),
                                                              ECDSA_SIG_free);
    if (!sig) return openSslFailure(ErrorCode::Internal, "can't read a signature");
    unsigned char raw[64];
    if (BN_bn2binpad(ECDSA_SIG_get0_r(sig.get()), raw, 32) != 32 ||
        BN_bn2binpad(ECDSA_SIG_get0_s(sig.get()), raw + 32, 32) != 32) {
        return openSslFailure(ErrorCode::Internal, "can't read a signature");
    }
    return std::string(reinterpret_cast<const char*>(raw), 64);
}

bool hostMatches(const std::string& pattern, const std::string& host) {
    std::string p = lower(pattern), h = lower(host);
    if (p.size() > 2 && p.compare(0, 2, "*.") == 0) {
        size_t dot = h.find('.');
        return dot != std::string::npos && dot > 0 && h.compare(dot, std::string::npos, p, 1, std::string::npos) == 0;
    }
    return p == h;
}

}  // namespace fourfold

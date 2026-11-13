// An ACME client (RFC 8555) for the server's certificates (overview §8,
// TLS): Let's Encrypt, or any CA that speaks ACME, with HTTP-01 challenges
// answered on port 80.
//
// It's synchronous, for a background thread (CertificateManager's). The
// HTTP it speaks goes through AcmeHttp, so tests can stand in for the CA.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/result.h"

namespace fourfold {

// Let's Encrypt's ACME directories.
extern const char* const kLetsEncryptDirectory;
extern const char* const kLetsEncryptStagingDirectory;

struct AcmeHttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;   // names lowercased
    std::string body;

    const std::string* header(const std::string& lowercaseName) const;
};

class AcmeHttp {
public:
    virtual ~AcmeHttp() = default;
    // GET, HEAD, or POST (with a body of type application/jose+json).
    virtual Result<AcmeHttpResponse> request(const std::string& method, const std::string& url,
                                             const std::string& body) = 0;
};

// Over libcurl. `caBundle`: the CA certificates to trust, as a file (for a
// test CA such as Pebble's); empty: the system's.
std::unique_ptr<AcmeHttp> makeCurlAcmeHttp(const std::string& caBundle = "");

// HTTP-01 challenge responses, by token: what port 80 serves at
// /.well-known/acme-challenge/<token>. Thread-safe.
class ChallengeBoard {
public:
    void put(const std::string& token, const std::string& keyAuthorization);
    void remove(const std::string& token);
    std::optional<std::string> get(const std::string& token) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> responses_;
};

struct AcmeOptions {
    std::string directoryUrl = kLetsEncryptDirectory;
    // The account's contact address ("mailto:" is added). Optional.
    std::string email;
    // How often to ask whether a challenge or order is done, and how many
    // times before giving up.
    std::chrono::milliseconds pollInterval{2000};
    int maxPolls = 60;
    // Waits between polls (tests make it instant).
    std::function<void(std::chrono::milliseconds)> sleep;
};

class AcmeClient {
public:
    // `accountKeyPem`: the account's key (P-256). `accountUrl`: the account
    // it's registered as, if known (else it's found or registered).
    AcmeClient(AcmeHttp& http, AcmeOptions options, std::string accountKeyPem, std::string accountUrl = "");

    // A certificate chain (PEM) for `names`, for the key `certificateKeyPem`.
    // Challenge responses go on `board` while they're needed.
    Result<std::string> obtain(const std::vector<std::string>& names, const std::string& certificateKeyPem,
                               ChallengeBoard& board);

    // The account's URL, once registered (to store, and pass next time).
    const std::string& accountUrl() const { return accountUrl_; }

private:
    struct Directory {
        std::string newNonce, newAccount, newOrder;
    };

    Status loadDirectory();
    Result<std::string> nonce();
    // A signed POST (payload empty: POST-as-GET). Retries once on badNonce.
    Result<AcmeHttpResponse> post(const std::string& url, const std::string& payload, bool useJwk = false);
    Status ensureAccount();
    Status authorize(const std::string& authorizationUrl, ChallengeBoard& board);
    // Polls `url` until its status is one of `done` (or "invalid").
    Result<std::string> pollUntil(const std::string& url, const std::vector<std::string>& done, std::string& body);
    void sleepFor(std::chrono::milliseconds d);

    AcmeHttp& http_;
    AcmeOptions options_;
    std::string key_;
    std::string jwk_;
    std::string thumbprint_;   // base64url SHA-256 of the JWK
    std::string accountUrl_;
    std::optional<Directory> directory_;
    std::string nonce_;
};

}  // namespace fourfold

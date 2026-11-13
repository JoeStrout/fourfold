// CertificateManager: keeps the server's certificate current (overview §8,
// TLS). On its own thread, it loads the certificate kept in
// private/state/tls/, installs it, and gets a new one from the CA (ACME)
// when there's none, it's for other names, or it has less than
// `renewBefore` or a third of its lifetime left, whichever is less; then
// checks again twice a day. A failure is logged and retried later, backing
// off (CAs limit failed attempts), while the old certificate, if any, keeps
// serving.
//
// In private/state/tls/: account.key and account.json (the ACME account, per
// directory URL), and <first name>.key and <first name>.crt.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "acme/acme_client.h"
#include "base/clock.h"
#include "partition/data_dir.h"
#include "tls/certificates.h"

namespace fourfold {

struct CertificateManagerOptions {
    // The names the certificate is for (the first names its files). Names
    // that ACME can't certify (IP addresses, localhost) are refused.
    std::vector<std::string> names;
    AcmeOptions acme;
    std::chrono::hours renewBefore{30 * 24};
    std::chrono::hours checkEvery{12};
    // Waits after failed attempts, in order; the last repeats.
    std::vector<std::chrono::milliseconds> retryDelays{std::chrono::minutes(1), std::chrono::minutes(10),
                                                       std::chrono::hours(1), std::chrono::hours(6)};
    // Installs a certificate chain and key (into the server's ServerTls).
    // Called on the manager's thread.
    std::function<Status(const std::string& chainPem, const std::string& keyPem)> install;
    std::function<void(const std::string&)> log;
    // The CA's HTTP; null: libcurl (with acmeCaBundle, if set).
    std::unique_ptr<AcmeHttp> http;
    std::string acmeCaBundle;
};

class CertificateManager {
public:
    static Result<std::unique_ptr<CertificateManager>> create(const StateDir& state,
                                                              CertificateManagerOptions options);
    ~CertificateManager();

    CertificateManager(const CertificateManager&) = delete;
    CertificateManager& operator=(const CertificateManager&) = delete;

    // Installs the certificate on disk at once, if there's a usable one
    // (so HTTPS works from the first connection), then starts the thread.
    void start();

    // What port 80 answers ACME's HTTP-01 challenges with.
    const ChallengeBoard& challenges() const { return board_; }

    // Does one round now: loads, and renews if needed. For tests, and the
    // thread. Returns whether a usable certificate is installed.
    bool checkNow();

    struct Status {
        std::optional<CertificateInfo> current;
        std::string lastError;
    };
    Status status() const;

    // Whether ACME can certify `name`: a DNS name, not an IP address or
    // localhost.
    static bool certifiable(const std::string& name);

private:
    CertificateManager(const StateDir& state, CertificateManagerOptions options);
    void run();
    // The installed certificate needs replacing.
    bool needsRenewal(const CertificateInfo& info) const;
    bool loadAndInstall();
    fourfold::Status obtain();
    void say(const std::string& line);

    const StateDir& state_;
    CertificateManagerOptions options_;
    std::unique_ptr<AcmeHttp> http_;
    ChallengeBoard board_;
    mutable std::mutex mutex_;
    std::optional<CertificateInfo> current_;
    std::string lastError_;
    size_t failures_ = 0;

    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace fourfold

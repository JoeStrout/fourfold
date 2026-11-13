#include "acme/certificate_manager.h"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "base/datum.h"
#include "platform/address.h"

namespace fourfold {

namespace {

const size_t kMaxFile = 256 * 1024;

RelPath tlsFile(const std::string& name) {
    return RelPath::parse("tls/" + name).value();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int64_t nowSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

// A file name for the certificate's first name ("*" can't be in one).
std::string fileStem(const std::string& name) {
    std::string s = name;
    std::replace(s.begin(), s.end(), '*', '_');
    return s;
}

}  // namespace

bool CertificateManager::certifiable(const std::string& name) {
    if (name.empty() || name.size() > 253 || lower(name) == "localhost") return false;
    std::string bare = name;
    if (bare.size() > 2 && bare.front() == '[' && bare.back() == ']') return false;
    if (parseAddress(bare)) return false;
    if (name.find('.') == std::string::npos) return false;   // a public name has a dot
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.')) return false;
    }
    return name.front() != '.' && name.back() != '.' && name.front() != '-';
}

Result<std::unique_ptr<CertificateManager>> CertificateManager::create(const StateDir& state,
                                                                       CertificateManagerOptions options) {
    if (options.names.empty()) return makeError(ErrorCode::Invalid, "no host names for a certificate");
    for (std::string& n : options.names) {
        n = lower(n);
        if (!certifiable(n)) {
            return makeError(ErrorCode::Invalid, "can't get a certificate for '" + n +
                                                     "' (Let's Encrypt certifies public DNS names only)");
        }
    }
    if (!options.install) return makeError(ErrorCode::Internal, "no certificate installer");
    return std::unique_ptr<CertificateManager>(new CertificateManager(state, std::move(options)));
}

CertificateManager::CertificateManager(const StateDir& state, CertificateManagerOptions options)
    : state_(state), options_(std::move(options)) {
    http_ = options_.http ? std::move(options_.http) : makeCurlAcmeHttp(options_.acmeCaBundle);
    if (!options_.acme.sleep) {
        // Waiting on the CA ends early when the manager stops.
        options_.acme.sleep = [this](std::chrono::milliseconds d) {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, d, [this] { return stopping_; });
        };
    }
}

CertificateManager::~CertificateManager() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void CertificateManager::say(const std::string& line) {
    if (options_.log) options_.log(line);
}

void CertificateManager::start() {
    loadAndInstall();
    thread_ = std::thread([this] { run(); });
}

CertificateManager::Status CertificateManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {current_, lastError_};
}

bool CertificateManager::needsRenewal(const CertificateInfo& info) const {
    // Every wanted name covered?
    for (const std::string& n : options_.names) {
        bool covered = false;
        for (const std::string& has : info.names) covered = covered || hostMatches(has, n);
        if (!covered) return true;
    }
    // A third of its life left, or renewBefore, whichever is sooner: a
    // 90-day certificate renews with 30 days left, a 6-day one with 2.
    int64_t renewBefore = std::chrono::duration_cast<std::chrono::seconds>(options_.renewBefore).count();
    int64_t lifetime = info.notAfter - info.notBefore;
    if (lifetime > 0) renewBefore = std::min(renewBefore, lifetime / 3);
    return info.notAfter - nowSeconds() < renewBefore;
}

bool CertificateManager::loadAndInstall() {
    std::string stem = fileStem(options_.names[0]);
    auto cert = state_.read(tlsFile(stem + ".crt"), kMaxFile);
    auto key = state_.read(tlsFile(stem + ".key"), kMaxFile);
    if (!cert || !key) return false;
    auto info = inspectCertificate(cert.value());
    if (!info || !checkKeyMatches(cert.value(), key.value()) || info.value().notAfter <= nowSeconds()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_ && current_->notAfter == info.value().notAfter && current_->names == info.value().names) {
            return true;   // already installed
        }
    }
    if (fourfold::Status s = options_.install(cert.value(), key.value()); !s) {
        say("can't use the certificate in private/state/tls/: " + s.error().message);
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = info.value();
    return true;
}

fourfold::Status CertificateManager::obtain() {
    // The account: its key, and its URL at this CA.
    auto accountKey = state_.read(tlsFile("account.key"), kMaxFile);
    std::string keyPem;
    if (accountKey) {
        keyPem = accountKey.value();
    } else {
        auto made = generatePrivateKey();
        if (!made) return made.error();
        keyPem = made.value();
        if (fourfold::Status s = state_.write(tlsFile("account.key"), keyPem); !s) return s;
    }
    std::string accountUrl;
    if (auto stored = state_.read(tlsFile("account.json"), kMaxFile)) {
        auto d = parseJson(stored.value());
        if (d && d.value().kind() == Datum::Kind::Map) {
            const auto& m = d.value().asMap();
            auto dir = m.find("directory"), url = m.find("url");
            if (dir != m.end() && url != m.end() && dir->second.kind() == Datum::Kind::String &&
                url->second.kind() == Datum::Kind::String && dir->second.asString() == options_.acme.directoryUrl) {
                accountUrl = url->second.asString();
            }
        }
    }

    // A fresh key for each certificate.
    auto certKey = generatePrivateKey();
    if (!certKey) return certKey.error();
    AcmeClient client(*http_, options_.acme, keyPem, accountUrl);
    say("asking " + options_.acme.directoryUrl + " for a certificate for " + [&] {
        std::string s;
        for (const std::string& n : options_.names) s += (s.empty() ? "" : ", ") + n;
        return s;
    }());
    auto chain = client.obtain(options_.names, certKey.value(), board_);
    if (client.accountUrl() != accountUrl && !client.accountUrl().empty()) {
        (void)state_.write(tlsFile("account.json"),
                           toJson(Datum::map({{"directory", Datum::string(options_.acme.directoryUrl)},
                                              {"url", Datum::string(client.accountUrl())}})) +
                               "\n");
    }
    if (!chain) return chain.error();

    // Install it, then keep it: key first, so a crash between the two
    // leaves a pair that doesn't match (ignored), not a certificate with
    // no key.
    if (fourfold::Status s = options_.install(chain.value(), certKey.value()); !s) return s;
    std::string stem = fileStem(options_.names[0]);
    if (fourfold::Status s = state_.write(tlsFile(stem + ".key"), certKey.value()); !s) return s;
    if (fourfold::Status s = state_.write(tlsFile(stem + ".crt"), chain.value()); !s) return s;
    auto info = inspectCertificate(chain.value());
    std::lock_guard<std::mutex> lock(mutex_);
    if (info) current_ = info.value();
    return {};
}

bool CertificateManager::checkNow() {
    loadAndInstall();
    std::optional<CertificateInfo> current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = current_;
    }
    if (current && !needsRenewal(*current)) return true;
    fourfold::Status s = obtain();
    std::lock_guard<std::mutex> lock(mutex_);
    if (s) {
        failures_ = 0;
        lastError_.clear();
        char date[32];
        time_t t = static_cast<time_t>(current_ ? current_->notAfter : 0);
        struct tm tm {};
        gmtime_r(&t, &tm);
        strftime(date, sizeof date, "%Y-%m-%d", &tm);
        say(std::string("got a certificate, valid until ") + date);
        return true;
    }
    failures_++;
    lastError_ = s.error().message;
    say("couldn't get a certificate: " + lastError_ +
        (current_ ? " (still using the current one)" : " (HTTPS doesn't work until there is one)"));
    return current_.has_value();
}

void CertificateManager::run() {
    for (;;) {
        checkNow();
        std::chrono::milliseconds wait;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failures_ == 0) {
                wait = options_.checkEvery;
            } else {
                const auto& delays = options_.retryDelays;
                wait = delays.empty() ? std::chrono::milliseconds(options_.checkEvery)
                                      : delays[std::min(failures_ - 1, delays.size() - 1)];
            }
        }
        std::unique_lock<std::mutex> lock(wakeMutex_);
        if (wake_.wait_for(lock, wait, [this] { return stopping_; })) return;
    }
}

}  // namespace fourfold

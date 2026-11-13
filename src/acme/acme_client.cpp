#include "acme/acme_client.h"

#include <curl/curl.h>

#include <cctype>
#include <thread>

#include "base/crypto.h"
#include "base/datum.h"
#include "platform/ca_bundle.h"
#include "tls/certificates.h"

namespace fourfold {

const char* const kLetsEncryptDirectory = "https://acme-v02.api.letsencrypt.org/directory";
const char* const kLetsEncryptStagingDirectory = "https://acme-staging-v02.api.letsencrypt.org/directory";

namespace {

constexpr size_t kMaxResponseBytes = 1024 * 1024;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

const Datum* field(const Datum& d, const std::string& name) {
    if (d.kind() != Datum::Kind::Map) return nullptr;
    auto it = d.asMap().find(name);
    return it == d.asMap().end() ? nullptr : &it->second;
}

std::string stringField(const Datum& d, const std::string& name) {
    const Datum* f = field(d, name);
    return f && f->kind() == Datum::Kind::String ? f->asString() : std::string();
}

Result<Datum> parseBody(const AcmeHttpResponse& r) {
    auto d = parseJson(r.body);
    if (!d) return makeError(ErrorCode::IO, "the CA's response isn't JSON (HTTP " + std::to_string(r.status) + ")");
    return std::move(d).value();
}

// "the CA said: …" from a problem document (RFC 7807), or the status.
std::string problem(const AcmeHttpResponse& r) {
    auto d = parseJson(r.body);
    std::string detail, type;
    if (d) {
        detail = stringField(d.value(), "detail");
        type = stringField(d.value(), "type");
        // Subproblems, per identifier.
        if (const Datum* subs = field(d.value(), "subproblems"); subs && subs->kind() == Datum::Kind::List) {
            for (const Datum& s : subs->asList()) {
                std::string sd = stringField(s, "detail");
                if (!sd.empty()) detail += "; " + sd;
            }
        }
    }
    std::string prefix = "urn:ietf:params:acme:error:";
    if (type.rfind(prefix, 0) == 0) type = type.substr(prefix.size());
    std::string out = "HTTP " + std::to_string(r.status);
    if (!type.empty()) out += " (" + type + ")";
    if (!detail.empty()) out += ": " + detail;
    return out;
}

// The problem's type without its "urn:ietf:params:acme:error:", or "".
std::string problemType(const AcmeHttpResponse& r) {
    if (r.status < 400) return "";
    auto d = parseJson(r.body);
    std::string type = d ? stringField(d.value(), "type") : "";
    std::string prefix = "urn:ietf:params:acme:error:";
    return type.rfind(prefix, 0) == 0 ? type.substr(prefix.size()) : "";
}

bool isBadNonce(const AcmeHttpResponse& r) {
    return r.status == 400 && problemType(r) == "badNonce";
}

// --- libcurl ---

struct CurlResponse {
    AcmeHttpResponse response;
    bool tooLarge = false;
};

size_t onBody(char* data, size_t size, size_t count, void* user) {
    auto* r = static_cast<CurlResponse*>(user);
    size_t n = size * count;
    if (r->response.body.size() + n > kMaxResponseBytes) {
        r->tooLarge = true;
        return 0;
    }
    r->response.body.append(data, n);
    return n;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
    auto* r = static_cast<CurlResponse*>(user);
    size_t n = size * count;
    std::string line(data, n);
    if (line.rfind("HTTP/", 0) == 0) r->response.headers.clear();   // a new response (after 100, or a redirect)
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        r->response.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return n;
}

class CurlAcmeHttp : public AcmeHttp {
public:
    explicit CurlAcmeHttp(std::string caBundle) : caBundle_(std::move(caBundle)) {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    Result<AcmeHttpResponse> request(const std::string& method, const std::string& url,
                                     const std::string& body) override {
        if (url.rfind("https://", 0) != 0) return makeError(ErrorCode::Invalid, "ACME URLs must be https: " + url);
        CURL* curl = curl_easy_init();
        if (!curl) return makeError(ErrorCode::IO, "can't start an HTTPS request");
        CurlResponse out;
        char error[CURL_ERROR_SIZE] = {0};
        curl_slist* headers = nullptr;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        if (!caBundle_.empty()) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle_.c_str());
        } else if (auto ca = caBundleFallback(curl_version_info(CURLVERSION_NOW)->cainfo)) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca->c_str());
        }
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Fourfold-ACME");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out);
        if (method == "HEAD") {
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        } else if (method == "POST") {
            headers = curl_slist_append(headers, "Content-Type: application/jose+json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (out.tooLarge) return makeError(ErrorCode::TooLarge, "the CA's response is too large");
        if (rc != CURLE_OK) {
            return makeError(ErrorCode::IO, "can't reach " + url + ": " +
                                                (error[0] ? std::string(error) : curl_easy_strerror(rc)));
        }
        out.response.status = static_cast<int>(status);
        return std::move(out.response);
    }

private:
    std::string caBundle_;
};

}  // namespace

const std::string* AcmeHttpResponse::header(const std::string& lowercaseName) const {
    auto it = headers.find(lowercaseName);
    return it == headers.end() ? nullptr : &it->second;
}

std::unique_ptr<AcmeHttp> makeCurlAcmeHttp(const std::string& caBundle) {
    return std::make_unique<CurlAcmeHttp>(caBundle);
}

void ChallengeBoard::put(const std::string& token, const std::string& keyAuthorization) {
    std::lock_guard<std::mutex> lock(mutex_);
    responses_[token] = keyAuthorization;
}

void ChallengeBoard::remove(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    responses_.erase(token);
}

std::optional<std::string> ChallengeBoard::get(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = responses_.find(token);
    if (it == responses_.end()) return std::nullopt;
    return it->second;
}

AcmeClient::AcmeClient(AcmeHttp& http, AcmeOptions options, std::string accountKeyPem, std::string accountUrl)
    : http_(http), options_(std::move(options)), key_(std::move(accountKeyPem)), accountUrl_(std::move(accountUrl)) {}

void AcmeClient::sleepFor(std::chrono::milliseconds d) {
    if (options_.sleep) {
        options_.sleep(d);
    } else {
        std::this_thread::sleep_for(d);
    }
}

Status AcmeClient::loadDirectory() {
    if (directory_) return {};
    if (jwk_.empty()) {
        auto jwk = publicJwk(key_);
        if (!jwk) return jwk.error();
        jwk_ = jwk.value();
        thumbprint_ = toBase64Url(sha256(jwk_));
    }
    auto r = http_.request("GET", options_.directoryUrl, "");
    if (!r) return r.error();
    if (r.value().status != 200) {
        return makeError(ErrorCode::IO, "the ACME directory " + options_.directoryUrl + ": " + problem(r.value()));
    }
    auto d = parseBody(r.value());
    if (!d) return d.error();
    Directory dir{stringField(d.value(), "newNonce"), stringField(d.value(), "newAccount"),
                  stringField(d.value(), "newOrder")};
    if (dir.newNonce.empty() || dir.newAccount.empty() || dir.newOrder.empty()) {
        return makeError(ErrorCode::IO, "the ACME directory " + options_.directoryUrl + " is missing URLs");
    }
    directory_ = dir;
    return {};
}

Result<std::string> AcmeClient::nonce() {
    if (!nonce_.empty()) {
        std::string n = std::move(nonce_);
        nonce_.clear();
        return n;
    }
    auto r = http_.request("HEAD", directory_->newNonce, "");
    if (!r) return r.error();
    const std::string* n = r.value().header("replay-nonce");
    if (!n || n->empty()) return makeError(ErrorCode::IO, "the CA gave no nonce");
    return *n;
}

Result<AcmeHttpResponse> AcmeClient::post(const std::string& url, const std::string& payload, bool useJwk) {
    for (int attempt = 0;; attempt++) {
        auto n = nonce();
        if (!n) return n.error();
        Datum::MapItems header{{"alg", Datum::string("ES256")},
                               {"nonce", Datum::string(n.value())},
                               {"url", Datum::string(url)}};
        if (useJwk) {
            header["jwk"] = parseJson(jwk_).value();
        } else {
            header["kid"] = Datum::string(accountUrl_);
        }
        std::string protected64 = toBase64Url(toJson(Datum::map(std::move(header))));
        std::string payload64 = toBase64Url(payload);
        auto sig = signEs256(key_, protected64 + "." + payload64);
        if (!sig) return sig.error();
        std::string body = toJson(Datum::map({{"protected", Datum::string(protected64)},
                                              {"payload", Datum::string(payload64)},
                                              {"signature", Datum::string(toBase64Url(sig.value()))}}));
        auto r = http_.request("POST", url, body);
        if (!r) return r;
        if (const std::string* next = r.value().header("replay-nonce")) nonce_ = *next;
        if (isBadNonce(r.value()) && attempt < 2) continue;
        return r;
    }
}

Status AcmeClient::ensureAccount() {
    if (!accountUrl_.empty()) return {};
    // JSON's true: Datum has no booleans, so this is written by hand.
    std::string contact;
    if (!options_.email.empty()) {
        contact = ",\"contact\":" + toJson(Datum::list({Datum::string("mailto:" + options_.email)}));
    }
    std::string payload = "{\"termsOfServiceAgreed\":true" + contact + "}";
    auto r = post(directory_->newAccount, payload, true);
    if (!r) return r.error();
    if (r.value().status != 200 && r.value().status != 201) {
        return makeError(ErrorCode::IO, "registering with the CA: " + problem(r.value()));
    }
    const std::string* location = r.value().header("location");
    if (!location || location->empty()) return makeError(ErrorCode::IO, "the CA gave the account no URL");
    accountUrl_ = *location;
    return {};
}

Result<std::string> AcmeClient::pollUntil(const std::string& url, const std::vector<std::string>& done,
                                          std::string& body) {
    for (int i = 0; i <= options_.maxPolls; i++) {
        auto r = post(url, "");
        if (!r) return r.error();
        if (r.value().status != 200) return makeError(ErrorCode::IO, "asking the CA: " + problem(r.value()));
        auto d = parseBody(r.value());
        if (!d) return d.error();
        std::string status = stringField(d.value(), "status");
        body = r.value().body;
        for (const std::string& s : done) {
            if (status == s) return status;
        }
        if (status == "invalid") return status;
        sleepFor(options_.pollInterval);
    }
    return makeError(ErrorCode::IO, "the CA didn't finish in time");
}

namespace {

// Why a challenge or authorization failed, from its "error" or its
// challenges' errors.
std::string failure(const std::string& body) {
    auto d = parseJson(body);
    if (!d) return "invalid";
    std::string why;
    auto describe = [&](const Datum& error) {
        std::string detail = stringField(error, "detail");
        if (!detail.empty()) why += (why.empty() ? "" : "; ") + detail;
    };
    if (const Datum* e = field(d.value(), "error")) describe(*e);
    if (const Datum* cs = field(d.value(), "challenges"); cs && cs->kind() == Datum::Kind::List) {
        for (const Datum& c : cs->asList()) {
            if (const Datum* e = field(c, "error")) describe(*e);
        }
    }
    return why.empty() ? "invalid" : why;
}

}  // namespace

Status AcmeClient::authorize(const std::string& authorizationUrl, ChallengeBoard& board) {
    auto r = post(authorizationUrl, "");
    if (!r) return r.error();
    if (r.value().status != 200) return makeError(ErrorCode::IO, "reading an authorization: " + problem(r.value()));
    auto d = parseBody(r.value());
    if (!d) return d.error();
    const Datum* id = field(d.value(), "identifier");
    std::string identifier = id ? stringField(*id, "value") : std::string();
    std::string status = stringField(d.value(), "status");
    if (status == "valid") return {};
    if (status != "pending") {
        return makeError(ErrorCode::IO, "the authorization for " + identifier + " is " + status);
    }
    std::string token, challengeUrl;
    if (const Datum* cs = field(d.value(), "challenges"); cs && cs->kind() == Datum::Kind::List) {
        for (const Datum& c : cs->asList()) {
            if (stringField(c, "type") == "http-01") {
                token = stringField(c, "token");
                challengeUrl = stringField(c, "url");
            }
        }
    }
    // Tokens are base64url (RFC 8555 §8.1); anything else would be odd in a
    // URL path.
    if (token.empty() || challengeUrl.empty() || !fromBase64Url(token)) {
        return makeError(ErrorCode::IO, "the CA offered no HTTP-01 challenge for " + identifier);
    }
    board.put(token, token + "." + thumbprint_);
    auto started = post(challengeUrl, "{}");
    Status result;
    if (!started) {
        result = started.error();
    } else if (started.value().status != 200) {
        result = makeError(ErrorCode::IO, "starting the challenge for " + identifier + ": " + problem(started.value()));
    } else {
        std::string body;
        auto final = pollUntil(authorizationUrl, {"valid"}, body);
        if (!final) {
            result = final.error();
        } else if (final.value() != "valid") {
            result = makeError(ErrorCode::IO, "the CA couldn't check " + identifier + ": " + failure(body));
        }
    }
    board.remove(token);
    return result;
}

Result<std::string> AcmeClient::obtain(const std::vector<std::string>& names, const std::string& certificateKeyPem,
                                       ChallengeBoard& board) {
    if (names.empty()) return makeError(ErrorCode::Invalid, "no host names to get a certificate for");
    if (Status s = loadDirectory(); !s) return s.error();
    if (Status s = ensureAccount(); !s) return s.error();

    Datum::ListItems identifiers;
    for (const std::string& n : names) {
        identifiers.push_back(Datum::map({{"type", Datum::string("dns")}, {"value", Datum::string(n)}}));
    }
    std::string orderPayload = toJson(Datum::map({{"identifiers", Datum::list(identifiers)}}));
    auto order = post(directory_->newOrder, orderPayload);
    if (!order) return order.error();
    if (problemType(order.value()) == "accountDoesNotExist") {
        // The CA doesn't know the stored account (a test CA restarted, or
        // the account was removed): register again.
        accountUrl_.clear();
        if (Status s = ensureAccount(); !s) return s.error();
        order = post(directory_->newOrder, orderPayload);
        if (!order) return order.error();
    }
    if (order.value().status != 201) return makeError(ErrorCode::IO, "ordering a certificate: " + problem(order.value()));
    const std::string* orderUrlHeader = order.value().header("location");
    if (!orderUrlHeader) return makeError(ErrorCode::IO, "the CA gave the order no URL");
    std::string orderUrl = *orderUrlHeader;
    auto orderBody = parseBody(order.value());
    if (!orderBody) return orderBody.error();
    std::string finalize = stringField(orderBody.value(), "finalize");
    if (const Datum* authz = field(orderBody.value(), "authorizations"); authz && authz->kind() == Datum::Kind::List) {
        for (const Datum& a : authz->asList()) {
            if (a.kind() != Datum::Kind::String) continue;
            if (Status s = authorize(a.asString(), board); !s) return s.error();
        }
    }

    std::string body;
    auto ready = pollUntil(orderUrl, {"ready", "valid"}, body);
    if (!ready) return ready.error();
    if (ready.value() == "invalid") return makeError(ErrorCode::IO, "the order failed: " + failure(body));
    if (ready.value() == "ready") {
        auto csr = certificateRequestDer(names, certificateKeyPem);
        if (!csr) return csr.error();
        auto f = post(finalize, toJson(Datum::map({{"csr", Datum::string(toBase64Url(csr.value()))}})));
        if (!f) return f.error();
        if (f.value().status != 200) return makeError(ErrorCode::IO, "finalizing the order: " + problem(f.value()));
        auto valid = pollUntil(orderUrl, {"valid"}, body);
        if (!valid) return valid.error();
        if (valid.value() != "valid") return makeError(ErrorCode::IO, "the order failed: " + failure(body));
    }
    auto finalOrder = parseJson(body);
    std::string certificateUrl = finalOrder ? stringField(finalOrder.value(), "certificate") : "";
    if (certificateUrl.empty()) return makeError(ErrorCode::IO, "the CA gave no certificate URL");
    auto cert = post(certificateUrl, "");
    if (!cert) return cert.error();
    if (cert.value().status != 200) {
        return makeError(ErrorCode::IO, "downloading the certificate: " + problem(cert.value()));
    }
    // Check it before anyone installs it.
    auto info = inspectCertificate(cert.value().body);
    if (!info) return makeError(ErrorCode::IO, "the CA's certificate can't be read: " + info.error().message);
    if (Status s = checkKeyMatches(cert.value().body, certificateKeyPem); !s) {
        return makeError(ErrorCode::IO, "the CA's certificate isn't for our key");
    }
    return cert.value().body;
}

}  // namespace fourfold

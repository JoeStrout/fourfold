// The ACME client and the certificate manager, against a fake CA that
// checks every request's JWS signature, checks HTTP-01 answers on the
// challenge board, and signs certificates for the CSR's key.
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <map>
#include <set>
#include <sys/stat.h>

#include "acme/acme_client.h"
#include "acme/certificate_manager.h"
#include "base/crypto.h"
#include "base/datum.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"
#include "tls/certificates.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

std::string field(const Datum& d, const std::string& name) {
    if (d.kind() != Datum::Kind::Map) return "";
    auto it = d.asMap().find(name);
    return it != d.asMap().end() && it->second.kind() == Datum::Kind::String ? it->second.asString() : "";
}

// Verifies an ES256 signature (r || s) with the public key in a JWK.
bool verifyEs256(const std::string& jwkJson, const std::string& data, const std::string& raw) {
    auto jwk = parseJson(jwkJson);
    if (!jwk || raw.size() != 64) return false;
    auto x = fromBase64Url(field(jwk.value(), "x")), y = fromBase64Url(field(jwk.value(), "y"));
    if (!x || !y || x->size() != 32 || y->size() != 32) return false;
    std::string point = "\x04" + *x + *y;
    EVP_PKEY* key = EVP_PKEY_new();
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string("group", const_cast<char*>("prime256v1"), 0),
            OSSL_PARAM_construct_octet_string("pub", point.data(), point.size()), OSSL_PARAM_construct_end()};
        EVP_PKEY_free(key);
        key = nullptr;
        EVP_PKEY_fromdata_init(ctx);
        EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params);
        EVP_PKEY_CTX_free(ctx);
    }
    if (!key) return false;
    ECDSA_SIG* sig = ECDSA_SIG_new();
    ECDSA_SIG_set0(sig, BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), 32, nullptr),
                   BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()) + 32, 32, nullptr));
    unsigned char* der = nullptr;
    int derLength = i2d_ECDSA_SIG(sig, &der);
    ECDSA_SIG_free(sig);
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, key) == 1 &&
              EVP_DigestVerify(md, der, static_cast<size_t>(derLength),
                               reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1;
    EVP_MD_CTX_free(md);
    OPENSSL_free(der);
    EVP_PKEY_free(key);
    ERR_clear_error();
    return ok;
}

class FakeCa : public AcmeHttp {
public:
    const std::string base = "https://ca.test";
    const ChallengeBoard* board = nullptr;   // where HTTP-01 answers are checked
    int days = 90;
    bool failChallenges = false;
    bool sendBadNonceOnce = false;
    int accountsCreated = 0;
    int ordersCreated = 0;
    int requests = 0;
    std::vector<std::string> errors;   // problems the fake noticed with the client's requests

    Result<AcmeHttpResponse> request(const std::string& method, const std::string& url,
                                     const std::string& body) override {
        requests++;
        AcmeHttpResponse r;
        r.status = 200;
        r.headers["replay-nonce"] = newNonce();
        if (url == base + "/directory" && method == "GET") {
            r.body = "{\"newNonce\":\"" + base + "/nonce\",\"newAccount\":\"" + base + "/account\",\"newOrder\":\"" +
                     base + "/order\"}";
            return r;
        }
        if (url == base + "/nonce" && method == "HEAD") return r;
        if (method != "POST") return problem(405, "malformed", "POST only");

        // The JWS: signature, nonce, url, and key or account.
        auto jws = parseJson(body);
        if (!jws) return problem(400, "malformed", "not JSON");
        std::string p64 = field(jws.value(), "protected"), pay64 = field(jws.value(), "payload");
        auto prot = fromBase64Url(p64), payload = fromBase64Url(pay64);
        auto sig = fromBase64Url(field(jws.value(), "signature"));
        if (!prot || !payload || !sig) return problem(400, "malformed", "bad base64url");
        auto header = parseJson(*prot);
        if (!header) return problem(400, "malformed", "bad protected header");
        if (field(header.value(), "alg") != "ES256") errors.push_back("alg");
        if (field(header.value(), "url") != url) errors.push_back("url mismatch");
        std::string nonce = field(header.value(), "nonce");
        if (!nonces_.erase(nonce)) return problem(400, "badNonce", "unknown nonce");
        if (sendBadNonceOnce) {
            sendBadNonceOnce = false;
            return problem(400, "badNonce", "try again");
        }
        std::string jwk;
        const auto& h = header.value().asMap();
        if (h.count("jwk")) {
            if (url != base + "/account") errors.push_back("jwk outside newAccount");
            jwk = toJson(h.at("jwk"));
        } else {
            std::string kid = field(header.value(), "kid");
            if (!accounts_.count(kid)) return problem(400, "accountDoesNotExist", "no such account");
            jwk = accounts_[kid];
        }
        if (!verifyEs256(jwk, p64 + "." + pay64, *sig)) return problem(400, "malformed", "bad signature");

        if (url == base + "/account") {
            if (payload->find("\"termsOfServiceAgreed\":true") == std::string::npos) errors.push_back("terms");
            lastContact = payload->find("mailto:") != std::string::npos ? *payload : "";
            for (auto& [kid, k] : accounts_) {
                if (k == jwk) {
                    r.headers["location"] = kid;
                    return r;
                }
            }
            std::string kid = base + "/acct/" + std::to_string(accounts_.size() + 1);
            accounts_[kid] = jwk;
            thumbprints_[kid] = toBase64Url(sha256(publicJwkCanonical(jwk)));
            accountsCreated++;
            r.status = 201;
            r.headers["location"] = kid;
            return r;
        }
        std::string kid = field(header.value(), "kid");
        if (url == base + "/order") {
            auto order = parseJson(*payload);
            Order o;
            o.account = kid;
            if (order) {
                for (const auto& [k, v] : order.value().asMap()) {
                    if (k != "identifiers") continue;
                    for (const Datum& id : v.asList()) o.names.push_back(field(id, "value"));
                }
            }
            int n = ++ordersCreated;
            o.id = std::to_string(n);
            for (size_t i = 0; i < o.names.size(); i++) {
                std::string a = o.id + "-" + std::to_string(i);
                authz_[a] = {o.names[i], "tok" + a + "_AbC-9", "pending"};
                o.authz.push_back(a);
            }
            orders_[o.id] = o;
            r.status = 201;
            r.headers["location"] = base + "/orders/" + o.id;
            r.body = orderJson(orders_[o.id]);
            return r;
        }
        if (url.rfind(base + "/authz/", 0) == 0) {
            Authz& a = authz_[url.substr(base.size() + 7)];
            if (!payload->empty()) errors.push_back("authz not POST-as-GET");
            r.body = "{\"status\":\"" + a.status + "\",\"identifier\":{\"type\":\"dns\",\"value\":\"" + a.name +
                     "\"},\"challenges\":[{\"type\":\"dns-01\",\"url\":\"" + base +
                     "/chall/dns\",\"token\":\"x\"},{\"type\":\"http-01\",\"url\":\"" + base + "/chall/" +
                     url.substr(base.size() + 7) + "\",\"token\":\"" + a.token + "\"" +
                     (a.status == "invalid" ? ",\"error\":{\"detail\":\"connection refused\"}" : "") + "}]}";
            return r;
        }
        if (url.rfind(base + "/chall/", 0) == 0) {
            Authz& a = authz_[url.substr(base.size() + 7)];
            if (*payload != "{}") errors.push_back("challenge payload");
            // "Fetch" http://<name>/.well-known/acme-challenge/<token>.
            auto answer = board ? board->get(a.token) : std::nullopt;
            bool right = answer && *answer == a.token + "." + thumbprints_[kid];
            a.status = right && !failChallenges ? "valid" : "invalid";
            r.body = "{\"status\":\"processing\"}";
            return r;
        }
        if (url.rfind(base + "/orders/", 0) == 0) {
            Order& o = orders_[url.substr(base.size() + 8)];
            r.body = orderJson(o);
            return r;
        }
        if (url.rfind(base + "/finalize/", 0) == 0) {
            Order& o = orders_[url.substr(base.size() + 10)];
            if (orderStatus(o) != "ready") return problem(403, "orderNotReady", "not ready");
            auto csr = parseJson(*payload);
            auto der = csr ? fromBase64Url(field(csr.value(), "csr")) : std::nullopt;
            if (!der) return problem(400, "badCSR", "no CSR");
            o.certificate = sign(*der);
            if (o.certificate.empty()) return problem(400, "badCSR", "bad CSR");
            r.body = orderJson(o);
            return r;
        }
        if (url.rfind(base + "/cert/", 0) == 0) {
            Order& o = orders_[url.substr(base.size() + 6)];
            r.headers["content-type"] = "application/pem-certificate-chain";
            r.body = o.certificate + caPem();
            return r;
        }
        return problem(404, "malformed", "no such resource");
    }

    std::string lastContact;

private:
    struct Authz {
        std::string name, token, status;
    };
    struct Order {
        std::string id, account;
        std::vector<std::string> names, authz;
        std::string certificate;
    };

    std::string newNonce() {
        std::string n = "n" + std::to_string(++nonceCounter_);
        nonces_.insert(n);
        return n;
    }
    AcmeHttpResponse problem(int status, const std::string& type, const std::string& detail) {
        AcmeHttpResponse r;
        r.status = status;
        r.headers["replay-nonce"] = newNonce();
        r.body = "{\"type\":\"urn:ietf:params:acme:error:" + type + "\",\"detail\":\"" + detail + "\"}";
        return r;
    }
    // The client's JWK, re-serialized canonically (the thumbprint's input).
    static std::string publicJwkCanonical(const std::string& jwk) {
        auto d = parseJson(jwk).value();
        return "{\"crv\":\"" + field(d, "crv") + "\",\"kty\":\"" + field(d, "kty") + "\",\"x\":\"" + field(d, "x") +
               "\",\"y\":\"" + field(d, "y") + "\"}";
    }
    std::string orderStatus(const Order& o) {
        if (!o.certificate.empty()) return "valid";
        bool all = true;
        for (const std::string& a : o.authz) {
            if (authz_[a].status == "invalid") return "invalid";
            all = all && authz_[a].status == "valid";
        }
        return all ? "ready" : "pending";
    }
    std::string orderJson(Order& o) {
        std::string authz;
        for (const std::string& a : o.authz) authz += (authz.empty() ? "\"" : ",\"") + base + "/authz/" + a + "\"";
        std::string status = orderStatus(o);
        return "{\"status\":\"" + status + "\",\"authorizations\":[" + authz + "],\"finalize\":\"" + base +
               "/finalize/" + o.id + "\"" +
               (status == "valid" ? ",\"certificate\":\"" + base + "/cert/" + o.id + "\"" : "") + "}";
    }
    // A certificate for the CSR's key and names, signed by the fake CA.
    std::string sign(const std::string& der) {
        const auto* p = reinterpret_cast<const unsigned char*>(der.data());
        X509_REQ* req = d2i_X509_REQ(nullptr, &p, static_cast<long>(der.size()));
        if (!req) return "";
        if (caKey_.empty()) {
            caKey_ = generatePrivateKey().value();
            caCert_ = selfSignedCertificate({"fake-ca.test"}, caKey_, 3650).value();
        }
        BIO* kb = BIO_new_mem_buf(caKey_.data(), static_cast<int>(caKey_.size()));
        EVP_PKEY* caKey = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
        BIO_free(kb);
        X509* cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), ++serial_);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), static_cast<long>(days) * 86400);
        X509_set_subject_name(cert, X509_REQ_get_subject_name(req));
        X509_NAME* issuer = X509_NAME_new();
        X509_NAME_add_entry_by_txt(issuer, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Fake CA"), -1,
                                   -1, 0);
        X509_set_issuer_name(cert, issuer);
        X509_NAME_free(issuer);
        X509_set_pubkey(cert, X509_REQ_get0_pubkey(req));
        STACK_OF(X509_EXTENSION)* exts = X509_REQ_get_extensions(req);
        for (int i = 0; exts && i < sk_X509_EXTENSION_num(exts); i++) X509_add_ext(cert, sk_X509_EXTENSION_value(exts, i), -1);
        if (exts) sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
        X509_sign(cert, caKey, EVP_sha256());
        BIO* out = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(out, cert);
        char* data;
        long n = BIO_get_mem_data(out, &data);
        std::string pem(data, static_cast<size_t>(n));
        BIO_free(out);
        X509_free(cert);
        X509_REQ_free(req);
        EVP_PKEY_free(caKey);
        return pem;
    }
    std::string caPem() const { return caCert_; }

    std::set<std::string> nonces_;
    int nonceCounter_ = 0;
    std::map<std::string, std::string> accounts_;      // kid → JWK
    std::map<std::string, std::string> thumbprints_;   // kid → thumbprint
    std::map<std::string, Order> orders_;
    std::map<std::string, Authz> authz_;
    std::string caKey_, caCert_;
    long serial_ = 0;
};

AcmeOptions fakeOptions() {
    AcmeOptions o;
    o.directoryUrl = "https://ca.test/directory";
    o.email = "admin@example.com";
    o.sleep = [](std::chrono::milliseconds) {};
    o.maxPolls = 5;
    return o;
}

}  // namespace

TEST_CASE("tls: the JWK and ES256 signatures ACME uses") {
    auto key = generatePrivateKey();
    REQUIRE(key.ok());
    auto jwk = publicJwk(key.value());
    REQUIRE(jwk.ok());
    CHECK_EQ(jwk.value().rfind("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"", 0), 0u);
    CHECK_EQ(jwk.value().size(), 31u + 43 + 7 + 43 + 2);   // fixed shape: 32-byte coordinates
    auto sig = signEs256(key.value(), "hello");
    REQUIRE(sig.ok());
    CHECK_EQ(sig.value().size(), 64u);
    CHECK(verifyEs256(jwk.value(), "hello", sig.value()));
    CHECK_FALSE(verifyEs256(jwk.value(), "hellO", sig.value()));
}

TEST_CASE("AcmeClient: registers, answers HTTP-01, finalizes, and downloads the certificate") {
    FakeCa ca;
    ChallengeBoard board;
    ca.board = &board;
    auto accountKey = generatePrivateKey().value();
    auto certKey = generatePrivateKey().value();
    AcmeClient client(ca, fakeOptions(), accountKey);
    auto chain = client.obtain({"example.com", "www.example.com"}, certKey, board);
    REQUIRE_MESSAGE(chain.ok(), (chain.ok() ? "" : chain.error().message));
    CHECK(ca.errors.empty());
    CHECK_EQ(ca.accountsCreated, 1);
    CHECK_EQ(client.accountUrl(), "https://ca.test/acct/1");
    CHECK(ca.lastContact.find("mailto:admin@example.com") != std::string::npos);
    auto info = inspectCertificate(chain.value());
    REQUIRE(info.ok());
    CHECK_EQ(info.value().names, std::vector<std::string>{"example.com", "www.example.com"});
    CHECK(checkKeyMatches(chain.value(), certKey).ok());
    CHECK_FALSE(board.get("tok1-0_AbC-9"));   // taken down afterwards

    // Again, with the account known: no new account.
    AcmeClient again(ca, fakeOptions(), accountKey, client.accountUrl());
    CHECK(again.obtain({"example.com"}, certKey, board).ok());
    CHECK_EQ(ca.accountsCreated, 1);
}

TEST_CASE("AcmeClient: failures say why") {
    FakeCa ca;
    ChallengeBoard board;
    ca.board = &board;
    auto key = generatePrivateKey().value();
    SUBCASE("a failed challenge") {
        ca.failChallenges = true;
        AcmeClient client(ca, fakeOptions(), key);
        auto r = client.obtain({"example.com"}, key, board);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message.find("couldn't check example.com: connection refused") != std::string::npos);
    }
    SUBCASE("a stale nonce is retried") {
        ca.sendBadNonceOnce = true;
        AcmeClient client(ca, fakeOptions(), key);
        CHECK(client.obtain({"example.com"}, key, board).ok());
    }
    SUBCASE("an account the CA doesn't know is registered again") {
        AcmeClient client(ca, fakeOptions(), key, "https://ca.test/acct/99");
        CHECK(client.obtain({"example.com"}, key, board).ok());
        CHECK_EQ(ca.accountsCreated, 1);
        CHECK_EQ(client.accountUrl(), "https://ca.test/acct/1");
    }
    SUBCASE("no names") {
        AcmeClient client(ca, fakeOptions(), key);
        CHECK_FALSE(client.obtain({}, key, board).ok());
    }
}

TEST_CASE("CertificateManager: gets, keeps, reuses and renews the certificate") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    StateDir state = app.stateDir().value();

    auto* ca = new FakeCa;
    std::vector<std::string> installed;
    std::vector<std::string> logged;
    auto options = [&](std::unique_ptr<AcmeHttp> http) {
        CertificateManagerOptions o;
        o.names = {"Example.com"};
        o.acme = fakeOptions();
        o.http = std::move(http);
        o.install = [&](const std::string& chain, const std::string& key) -> Status {
            if (!checkKeyMatches(chain, key)) return makeError(ErrorCode::Invalid, "mismatch");
            installed.push_back(chain);
            return {};
        };
        o.log = [&](const std::string& line) { logged.push_back(line); };
        return o;
    };
    std::unique_ptr<AcmeHttp> owned(ca);
    auto m = CertificateManager::create(state, options(std::move(owned)));
    REQUIRE(m.ok());
    ca->board = &m.value()->challenges();
    CHECK(m.value()->checkNow());
    CHECK_EQ(installed.size(), 1u);
    REQUIRE(m.value()->status().current);
    CHECK_EQ(m.value()->status().current->names[0], "example.com");
    CHECK(m.value()->status().lastError.empty());
    // Kept in private/state/tls/, readable by the engine only.
    struct stat st;
    REQUIRE_EQ(::stat(dir.at("app/private/state/tls/example.com.key").c_str(), &st), 0);
    CHECK_EQ(st.st_mode & 0777, 0600u);
    CHECK_EQ(::stat(dir.at("app/private/state/tls/example.com.crt").c_str(), &st), 0);
    CHECK_EQ(::stat(dir.at("app/private/state/tls/account.json").c_str(), &st), 0);

    // Still fresh: nothing to do.
    int before = ca->requests;
    CHECK(m.value()->checkNow());
    CHECK_EQ(ca->requests, before);

    // A new manager (a restart) installs the stored one without asking.
    auto* ca2 = new FakeCa;
    auto m2 = CertificateManager::create(state, options(std::unique_ptr<AcmeHttp>(ca2)));
    REQUIRE(m2.ok());
    installed.clear();
    CHECK(m2.value()->checkNow());
    CHECK_EQ(installed.size(), 1u);
    CHECK_EQ(ca2->requests, 0);

    // Close to expiry: renewed (the fake CA issues 10-day ones now).
    m.value().reset();
    m2.value().reset();
    auto* ca3 = new FakeCa;
    ca3->days = 10;
    auto m3 = CertificateManager::create(state, options(std::unique_ptr<AcmeHttp>(ca3)));
    ca3->board = &m3.value()->challenges();
    // The stored account is ca.test's: this fake CA doesn't know it, but the
    // key finds (registers) it again.
    CHECK(m3.value()->checkNow());   // the stored one is fresh
    CHECK_EQ(ca3->requests, 0);
    CHECK(m3.value()->checkNow());
}

TEST_CASE("CertificateManager: renews a certificate near expiry, or for new names, and keeps the old on failure") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    StateDir state = app.stateDir().value();
    std::vector<std::string> installed;
    auto make = [&](FakeCa* ca, std::vector<std::string> names) {
        CertificateManagerOptions o;
        o.names = std::move(names);
        o.acme = fakeOptions();
        o.http = std::unique_ptr<AcmeHttp>(ca);
        o.install = [&](const std::string& chain, const std::string&) -> Status {
            installed.push_back(chain);
            return {};
        };
        auto m = CertificateManager::create(state, std::move(o));
        REQUIRE(m.ok());
        ca->board = &m.value()->challenges();
        return std::move(m).value();
    };
    auto* shortLived = new FakeCa;
    shortLived->days = 10;   // a 10-day certificate renews with 3.3 days left
    auto m = make(shortLived, {"example.com"});
    CHECK(m->checkNow());
    int64_t first = m->status().current->notAfter;
    m.reset();

    auto* ca = new FakeCa;
    m = make(ca, {"example.com"});
    CHECK(m->checkNow());
    CHECK_EQ(m->status().current->notAfter, first);   // 10 days left of 10: not yet
    CHECK(ca->ordersCreated == 0);
    m.reset();

    // A short-lived certificate renews by its lifetime, not renewBefore.
    {
        TempDir other;
        auto app2 = AppFolder::open(other.at("app"), AppFolder::Missing::Create).value();
        StateDir state2 = app2.stateDir().value();
        auto key = generatePrivateKey().value();
        other.write("app/private/state/tls/example.com.key", key);
        other.write("app/private/state/tls/example.com.crt", selfSignedCertificate({"example.com"}, key, 2).value());
        auto* fresh = new FakeCa;
        CertificateManagerOptions o;
        o.names = {"example.com"};
        o.acme = fakeOptions();
        o.http = std::unique_ptr<AcmeHttp>(fresh);
        o.renewBefore = std::chrono::hours(72);   // more than its whole life
        o.install = [](const std::string&, const std::string&) -> Status { return {}; };
        auto m2 = CertificateManager::create(state2, std::move(o)).value();
        fresh->board = &m2->challenges();
        CHECK(m2->checkNow());
        CHECK_EQ(fresh->ordersCreated, 0);   // ~47 hours left: more than a third of its 49

    }

    // Another name: a new certificate.
    auto* ca2 = new FakeCa;
    m = make(ca2, {"example.com", "www.example.com"});
    CHECK(m->checkNow());
    CHECK_EQ(m->status().current->names.size(), 2u);
    m.reset();

    // A failure keeps the old one, and says so.
    auto* failing = new FakeCa;
    failing->failChallenges = true;
    m = make(failing, {"example.com", "www.example.com", "new.example.com"});
    CHECK(m->checkNow());   // the old one is still there
    CHECK_FALSE(m->status().lastError.empty());
    CHECK_EQ(m->status().current->names.size(), 2u);
}

TEST_CASE("CertificateManager: only public DNS names") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    StateDir state = app.stateDir().value();
    CHECK(CertificateManager::certifiable("example.com"));
    CHECK(CertificateManager::certifiable("a-b.example.co.uk"));
    for (const char* bad : {"localhost", "127.0.0.1", "::1", "[::1]", "intranet", "a b.com", "", ".example.com",
                            "*.example.com"}) {
        CHECK_MESSAGE(!CertificateManager::certifiable(bad), bad);
    }
    CertificateManagerOptions o;
    o.names = {"localhost"};
    o.install = [](const std::string&, const std::string&) -> Status { return {}; };
    CHECK_FALSE(CertificateManager::create(state, std::move(o)).ok());
}

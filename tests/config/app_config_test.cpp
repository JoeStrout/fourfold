#include "config/app_config.h"

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

struct Fixture {
    TempDir dir;
    std::unique_ptr<ScriptHost> host = ScriptHost::create().value();
    std::vector<std::string> mailed;

    Fixture() { dir.makeDir("app/private/config"); }

    void write(const std::string& name, const std::string& text) { dir.write("app/private/config/" + name, text); }

    ConfigLoad load(ConfigMode mode = ConfigMode::Dev, const std::string& server = "") {
        auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
        REQUIRE(app.ok());
        auto config = app.value().configDir();
        REQUIRE(config.ok());
        ConfigSources sources;
        sources.dir = &config.value();
        sources.mode = mode;
        sources.devMailOutput = [this](const std::string& text) { mailed.push_back(text); };
        if (!server.empty()) {
            dir.write("server.ms", server);
            sources.serverConfig = ConfigDir::readServerConfig(dir.at("server.ms")).value();
        }
        return loadConfig(sources, *host);
    }
};

std::string all(const ConfigLoad& load) {
    std::string s;
    for (const Diagnostic& d : load.diagnostics) s += d.toString() + "\n";
    return s;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("config: no files at all means the defaults") {
    Fixture f;
    ConfigLoad dev = f.load();
    REQUIRE_MESSAGE(dev.ok(), all(dev));
    CHECK(dev.diagnostics.empty());
    CHECK(dev.config->auth.fakeLogin);
    CHECK(dev.config->auth.magicLink);
    CHECK(dev.config->auth.openSignUp);
    CHECK(dev.config->mail.transport == MailConfig::Transport::Log);
    REQUIRE(dev.config->mailer);
    REQUIRE(dev.config->mailer->send({dev.config->mail.from, "a@b.co", "Hi", "text", std::nullopt}));
    CHECK_EQ(f.mailed.size(), 1u);
    CHECK_EQ(dev.config->session.maxBytes, 64u * 1024);

    ConfigLoad serve = f.load(ConfigMode::Serve);
    REQUIRE_MESSAGE(serve.ok(), all(serve));
    CHECK_FALSE(serve.config->auth.fakeLogin);
    CHECK(serve.config->mail.transport == MailConfig::Transport::None);
    CHECK_FALSE(serve.config->mailer);
}

TEST_CASE("config: app.ms settings") {
    Fixture f;
    f.write("secrets.ms", "secrets = {}\nsecrets.smtpPassword = {\"value\": \"hunter2\"}\n"
                          "secrets.weatherKey = {\"value\": \"abc\", \"hosts\": [\"api.weather.gov\"]}\n");
    f.write("app.ms",
            "config = {}\n"
            "config.domain = \"Club.Example.com\"\n"
            "config.budget = {\"instructions\": 5000000, \"seconds\": 2.5}\n"
            "config.session = {\"capKB\": 32, \"userIdleDays\": 7}\n"
            "config.auth = {\"admins\": [\"Joe@Example.com\"], \"signUp\": \"closed\", \"linkMinutes\": 10}\n"
            "config.mail = {\"transport\": \"smtp\", \"from\": \"Club <noreply@example.com>\",\n"
            "  \"smtpHost\": \"smtp.example.com\", \"smtpUser\": \"noreply\", \"passwordSecret\": \"smtpPassword\"}\n");
    ConfigLoad load = f.load(ConfigMode::Serve);
    REQUIRE_MESSAGE(load.ok(), all(load));
    const AppConfig& c = *load.config;
    CHECK_EQ(c.site.domain, "club.example.com");
    CHECK_EQ(c.budget.instructions, 5'000'000u);
    CHECK_EQ(c.budget.wallClock.count(), 2500);
    CHECK_EQ(c.session.maxBytes, 32u * 1024);
    CHECK_EQ(c.session.signedInIdle, std::chrono::hours(24 * 7));
    CHECK_EQ(c.session.signedInLimit, std::chrono::hours(24 * 30));
    CHECK_EQ(c.auth.admins, std::vector<std::string>{"joe@example.com"});
    CHECK_FALSE(c.auth.openSignUp);
    CHECK_EQ(c.auth.store.tokenLifetime, std::chrono::minutes(10));
    CHECK(c.mail.transport == MailConfig::Transport::Smtp);
    CHECK(c.mailer);
    CHECK_EQ(c.secrets.at("weatherKey").hosts, std::vector<std::string>{"api.weather.gov"});
}

TEST_CASE("config: mistakes are errors, with the file named") {
    auto check = [](const std::string& app, const std::string& expected, ConfigMode mode = ConfigMode::Dev) {
        CAPTURE(app);
        Fixture f;
        f.write("secrets.ms", "secrets = {\"pw\": {\"value\": \"x\"}, \"shared\": {\"value\": \"y\", \"hosts\": [\"a.com\"]}}\n");
        f.write("app.ms", app);
        ConfigLoad load = f.load(mode);
        CHECK_FALSE(load.ok());
        CHECK_MESSAGE(contains(all(load), expected), all(load));
    };
    check("x = 1\n", "private/config/app.ms: error: must set `config`");
    check("config = {\"domian\": \"x.com\"}\n", "unknown setting config.domian");
    check("config = {\"auth\": {\"admin\": []}}\n", "unknown setting config.auth.admin");
    check("config = {\"domain\": \"https://x.com\"}\n", "config.domain must be a host name");
    check("config = {\"budget\": {\"seconds\": -1}}\n", "config.budget.seconds must be a number from");
    check("config = {\"budget\": {\"memoryMB\": 64}}\n", "isn't supported yet");
    check("config = {\"auth\": {\"signUp\": \"maybe\"}}\n", "must be \"open\" or \"closed\"");
    check("config = {\"auth\": {\"admins\": [\"not an email\"]}}\n", "isn't an email address");
    check("config = {\"auth\": {\"methods\": [\"password\"]}}\n", "the only sign-in method");
    check("config = {\"auth\": {\"fakeLogin\": true}}\n", "can't be on with `fourfold serve`", ConfigMode::Serve);
    check("config = {\"mail\": {\"transport\": \"smtp\", \"from\": \"a@b.co\"}}\n", "config.mail.smtpHost must be");
    check("config = {\"mail\": {\"transport\": \"smtp\", \"smtpHost\": \"mail.x.com\"}}\n", "config.mail.from is needed");
    check("config = {\"mail\": {\"transport\": \"smtp\", \"from\": \"a@b.co\", \"smtpHost\": \"mail.x.com\", "
          "\"smtpSecurity\": \"none\"}}\n",
          "only for a server on this machine");
    check("config = {\"mail\": {\"transport\": \"smtp\", \"from\": \"a@b.co\", \"smtpHost\": \"mail.x.com\", "
          "\"smtpUser\": \"u\", \"passwordSecret\": \"nope\"}}\n",
          "which isn't in private/config/secrets.ms");
    check("config = {\"mail\": {\"transport\": \"smtp\", \"from\": \"a@b.co\", \"smtpHost\": \"mail.x.com\", "
          "\"smtpUser\": \"u\", \"passwordSecret\": \"shared\"}}\n",
          "visible to app code");
    check("config = {\"mail\": {\"transport\": \"postmark\", \"from\": \"a@b.co\"}}\n", "postmarkTokenSecret is needed");
    check("config = {\"mail\": {\"transport\": \"smtp\", \"from\": \"a@b.co\", \"smtpHost\": \"mail.x.com\"}}\n",
          "config.domain is needed", ConfigMode::Serve);
    check("config = data.get(\"x\")\n", "can't be used");
}

TEST_CASE("config: secrets.ms") {
    Fixture f;
    f.write("secrets.ms", "secrets = {\"bad name\": {\"value\": \"x\"}, \"noValue\": {}, \"extra\": {\"value\": \"v\", \"x\": 1}}\n");
    ConfigLoad load = f.load();
    CHECK_FALSE(load.ok());
    std::string s = all(load);
    CHECK(contains(s, "private/config/secrets.ms: error: secrets.bad name: a secret's name"));
    CHECK(contains(s, "secrets.noValue.value is missing"));
    CHECK(contains(s, "unknown setting secrets.extra.x"));
}

TEST_CASE("config: the server's ceilings only lower values") {
    Fixture f;
    f.write("app.ms", "config = {\"budget\": {\"instructions\": 50000000, \"seconds\": 1}, \"session\": {\"capKB\": 32}}\n");
    ConfigLoad load = f.load(ConfigMode::Dev,
                             "server = {\"ceilings\": {\"budget\": {\"instructions\": 20000000, \"seconds\": 5},"
                             " \"session\": {\"capKB\": 16, \"anonTotal\": 500}}}\n");
    REQUIRE_MESSAGE(load.ok(), all(load));
    CHECK_EQ(load.config->budget.instructions, 20'000'000u);   // lowered, with a warning
    CHECK_EQ(load.config->budget.wallClock.count(), 1000);     // under the ceiling: as set
    CHECK_EQ(load.config->session.maxBytes, 16u * 1024);
    CHECK_EQ(load.config->session.maxAnonymous, 500u);         // the default, lowered quietly
    std::string s = all(load);
    CHECK(contains(s, "warning: config.budget.instructions is 50000000, over the server's ceiling of 20000000"));
    CHECK(contains(s, "config.session.capKB is 32"));
    CHECK_FALSE(contains(s, "anonTotal"));

    ConfigLoad bad = f.load(ConfigMode::Dev, "server = {\"ceilings\": {\"budget\": {\"instructionz\": 1}}}\n");
    CHECK_FALSE(bad.ok());
    CHECK(contains(all(bad), "unknown setting server.ceilings.budget.instructionz"));
}

TEST_CASE("config: limits and trusted proxies") {
    Fixture f;
    ConfigLoad defaults = f.load(ConfigMode::Serve);
    REQUIRE_MESSAGE(defaults.ok(), all(defaults));
    REQUIRE_EQ(defaults.config->limits.anonymous.size(), 1u);
    CHECK_EQ(defaults.config->limits.anonymous[0].describe(), "60/minute per ip");
    CHECK_EQ(defaults.config->limits.anonymous[0].key, "default");
    CHECK_EQ(defaults.config->limits.signedIn[0].describe(), "300/minute per user");
    CHECK_EQ(defaults.config->limits.challengeBits, 16);
    CHECK(defaults.config->proxy.trust.empty());
    CHECK_EQ(f.load(ConfigMode::Dev).config->limits.challengeBits, 4);   // quick to solve in dev

    f.write("app.ms", "config = {}\n"
                      "config.limits = {\"anonymous\": [\"20/minute\", \"500/day\"], \"signedIn\": \"100/minute\", "
                      "\"challengeBits\": 18}\n"
                      "config.proxy = {\"trust\": [\"127.0.0.1\", \"10.0.0.0/8\", \"::1\"]}\n");
    ConfigLoad set = f.load(ConfigMode::Serve);
    REQUIRE_MESSAGE(set.ok(), all(set));
    REQUIRE_EQ(set.config->limits.anonymous.size(), 2u);
    CHECK_EQ(set.config->limits.anonymous[1].describe(), "500/day per ip");
    CHECK_EQ(set.config->limits.signedIn[0].describe(), "100/minute per user");
    CHECK_EQ(set.config->limits.challengeBits, 18);
    REQUIRE_EQ(set.config->proxy.trust.size(), 3u);
    CHECK(set.config->proxy.trust[1].contains("10.20.30.40"));

    for (const char* bad : {"config.limits = {\"anonymous\": \"lots\"}",
                            "config.limits = {\"anonymous\": \"5/minute per user\"}",
                            "config.limits = {\"challengeBits\": 99}",
                            "config.limits = {\"burst\": 5}",
                            "config.proxy = {\"trust\": [\"proxy.example.com\"]}",
                            "config.proxy = {\"trust\": \"127.0.0.1\"}"}) {
        CAPTURE(bad);
        f.write("app.ms", std::string("config = {}\n") + bad + "\n");
        CHECK_FALSE(f.load(ConfigMode::Serve).ok());
    }
}

TEST_CASE("config: outbound, secrets for app code, and app mail (M9)") {
    Fixture f;
    f.write("secrets.ms", "secrets = {}\n"
                          "secrets.weatherKey = {\"value\": \"k\", \"hosts\": [\"API.weather.gov\", \"*.stripe.com\"]}\n"
                          "secrets.hookKey = {\"value\": \"h\", \"verify\": true}\n"
                          "secrets.smtpPassword = {\"value\": \"p\"}\n");
    f.write("app.ms", "config = {}\n"
                      "config.outbound = {\"enabled\": true, \"allow\": [\"api.weather.gov\"], "
                      "\"allowAddresses\": [\"10.1.2.0/24\"], \"perMinute\": 30, \"maxResponseKB\": 512, "
                      "\"timeoutSeconds\": 4}\n"
                      "config.mail = {\"appMail\": true, \"appPerHour\": 20}\n");
    ConfigLoad load = f.load();
    REQUIRE_MESSAGE(load.ok(), all(load));
    const AppConfig& c = *load.config;
    CHECK(c.secrets.at("weatherKey").hosts == std::vector<std::string>{"api.weather.gov", "*.stripe.com"});
    CHECK(c.secrets.at("weatherKey").allowsHost("api.stripe.com"));
    CHECK(!c.secrets.at("weatherKey").allowsHost("stripe.com"));
    CHECK(c.secrets.at("hookKey").verify);
    CHECK(c.secrets.at("smtpPassword").engineOnly());
    REQUIRE(c.outbound.cap);
    CHECK(c.outbound.cap->size() == 1);
    CHECK(c.outbound.allowAddresses.size() == 1);
    CHECK(c.outbound.perMinute == 30);
    CHECK(c.outbound.maxResponseBytes == 512u * 1024);
    CHECK(c.outbound.timeout == std::chrono::milliseconds(4000));
    CHECK(c.mail.appPerHour == 20);

    // A secret app code can use can't also be an engine secret.
    f.write("app.ms", "config = {}\nconfig.mail = {\"transport\": \"smtp\", \"from\": \"a@b.co\", "
                      "\"smtpHost\": \"smtp.b.co\", \"smtpUser\": \"u\", \"passwordSecret\": \"hookKey\"}\n");
    load = f.load();
    CHECK(!load.ok());
    CHECK(contains(all(load), "visible to app code"));

    for (const char* bad : {"config.outbound = {\"allow\": [\"https://x.com/path\"]}\n",
                            "config.outbound = {\"allowAddresses\": [\"not an address\"]}\n",
                            "config.outbound = {\"perMinute\": 0}\n",
                            "config.outbound = {\"widen\": true}\n"}) {
        f.write("app.ms", std::string("config = {}\n") + bad);
        load = f.load();
        CHECK_MESSAGE(!load.ok(), bad);
    }
    f.write("app.ms", "config = {}\n");
    f.write("secrets.ms", "secrets = {}\nsecrets.x = {\"value\": \"v\", \"hosts\": [\"*.com\"]}\n");
    load = f.load();
    CHECK(!load.ok());
}

TEST_CASE("config: tls (M11)") {
    Fixture f;
    CHECK(f.load(ConfigMode::Serve).config->tls.acmeDirectory.empty());   // Let's Encrypt, by default
    f.write("app.ms",
            "config = {}\n"
            "config.tls = {\"acme\": \"letsencrypt-staging\", \"email\": \"ops@example.com\", \"http2\": false}\n");
    auto load = f.load(ConfigMode::Serve);
    REQUIRE_MESSAGE(load.ok(), all(load));
    CHECK(contains(load.config->tls.acmeDirectory, "acme-staging-v02"));
    CHECK_EQ(load.config->tls.email, "ops@example.com");
    CHECK_FALSE(load.config->tls.http2);

    f.write("app.ms", "config = {}\nconfig.tls = {\"certFile\": \"cert.pem\", \"keyFile\": \"key.pem\"}\n");
    load = f.load(ConfigMode::Serve);
    REQUIRE(load.ok());
    CHECK_EQ(load.config->tls.certFile, "cert.pem");

    for (const char* bad : {"config.tls = {\"acme\": \"http://ca.test/dir\"}",
                            "config.tls = {\"email\": \"nobody\"}",
                            "config.tls = {\"certFile\": \"tls/cert.pem\", \"keyFile\": \"key.pem\"}",
                            "config.tls = {\"certFile\": \"cert.pem\"}",
                            "config.tls = {\"acmeCaBundle\": \"relative.pem\"}",
                            "config.tls = {\"colour\": 1}"}) {
        f.write("app.ms", std::string("config = {}\n") + bad + "\n");
        CHECK_MESSAGE(!f.load(ConfigMode::Serve).ok(), bad);
    }
}

TEST_CASE("config: headers, with CSP additions (M11)") {
    Fixture f;
    auto defaults = f.load(ConfigMode::Serve);
    const HeadersConfig& d = defaults.config->headers;
    std::string csp = d.cspBeforeNonce + " 'nonce-N'" + d.cspAfterNonce;
    CHECK_EQ(csp,
             "default-src 'self'; script-src 'self' 'nonce-N'; style-src 'self' 'unsafe-inline'; "
             "img-src 'self' https: data:; connect-src 'self'; object-src 'none'; base-uri 'none'; "
             "form-action 'self'; frame-ancestors 'none'; report-uri /__fourfold/csp-report; report-to csp");
    CHECK_EQ(d.frameOptions, "DENY");
    CHECK_EQ(d.hsts, "max-age=31536000");
    CHECK(contains(d.permissionsPolicy, "camera=()"));

    f.write("app.ms",
            "config = {}\n"
            "config.headers = {}\n"
            "config.headers.csp = {\"script\": [\"https://cdn.example.com\"], \"font\": [\"fonts.gstatic.com\"],\n"
            "                      \"img\": [\"blob:\"], \"connect\": [\"wss://push.example.com\"]}\n"
            "config.headers.strictStyles = true\n"
            "config.headers.frameAncestors = [\"https://partner.example\"]\n"
            "config.headers.hsts = {\"maxAgeDays\": 730, \"includeSubDomains\": true, \"preload\": true}\n"
            "config.headers.permissions = [\"geolocation\", \"fullscreen\"]\n");
    auto load = f.load(ConfigMode::Serve);
    REQUIRE_MESSAGE(load.ok(), all(load));
    const HeadersConfig& h = load.config->headers;
    CHECK_EQ(h.cspBeforeNonce, "default-src 'self'; script-src 'self' https://cdn.example.com");
    CHECK(contains(h.cspAfterNonce, "; style-src 'self'; "));
    CHECK(contains(h.cspAfterNonce, "img-src 'self' https: data: blob:"));
    CHECK(contains(h.cspAfterNonce, "connect-src 'self' wss://push.example.com"));
    CHECK(contains(h.cspAfterNonce, "font-src 'self' fonts.gstatic.com"));
    CHECK(contains(h.cspAfterNonce, "frame-ancestors https://partner.example"));
    CHECK(h.frameOptions.empty());
    CHECK_EQ(h.hsts, "max-age=63072000; includeSubDomains; preload");
    CHECK(contains(h.permissionsPolicy, "geolocation=(self)"));
    CHECK_FALSE(contains(h.permissionsPolicy, "geolocation=()"));
    CHECK(contains(h.permissionsPolicy, "camera=()"));

    f.write("app.ms", "config = {}\nconfig.headers = {\"frameAncestors\": [\"self\"], \"hsts\": false}\n");
    load = f.load(ConfigMode::Serve);
    REQUIRE(load.ok());
    CHECK_EQ(load.config->headers.frameOptions, "SAMEORIGIN");
    CHECK(load.config->headers.hsts.empty());

    for (const char* bad : {"config.headers = {\"csp\": {\"script\": [\"'unsafe-inline'\"]}}",
                            "config.headers = {\"csp\": {\"script\": [\"'unsafe-eval'\"]}}",
                            "config.headers = {\"csp\": {\"script\": [\"https:\"]}}",
                            "config.headers = {\"csp\": {\"script\": [\"*\"]}}",
                            "config.headers = {\"csp\": {\"style\": [\"a; script-src *\"]}}",
                            "config.headers = {\"csp\": {\"img\": [\"javascript:\"]}}",
                            "config.headers = {\"csp\": {\"worker\": [\"x.com\"]}}",
                            "config.headers = {\"frameAncestors\": [\"partner.example\"]}",
                            "config.headers = {\"hsts\": {\"preload\": true}}",
                            "config.headers = {\"permissions\": [\"telepathy\"]}"}) {
        f.write("app.ms", std::string("config = {}\n") + bad + "\n");
        CHECK_MESSAGE(!f.load(ConfigMode::Serve).ok(), bad);
    }
}

TEST_CASE("config: server limits and logging, within the server's ceilings (M11)") {
    Fixture f;
    auto defaults = f.load(ConfigMode::Serve);
    CHECK_EQ(defaults.config->server.connections, 0u);   // sized to the machine
    CHECK_EQ(defaults.config->server.requests, 64u);
    CHECK_EQ(defaults.config->logging.keepDays, 30);
    f.write("app.ms",
            "config = {}\n"
            "config.server = {\"connections\": 5000, \"connectionsPerAddress\": 50, \"requests\": 200,\n"
            "                 \"headerSeconds\": 5, \"keepAliveSeconds\": 30, \"minUploadBytesPerSecond\": 512,\n"
            "                 \"streamsPerAddress\": 4, \"requestWaitSeconds\": 2}\n"
            "config.logging = {\"keepDays\": 90}\n");
    auto load = f.load(ConfigMode::Serve, "server = {\"ceilings\": {\"server\": {\"requests\": 100}}}\n");
    REQUIRE_MESSAGE(load.ok(), all(load));
    const ServerConfig& s = load.config->server;
    CHECK_EQ(s.connections, 5000u);
    CHECK_EQ(s.connectionsPerAddress, 50u);
    CHECK_EQ(s.requests, 100u);   // the ceiling
    CHECK(contains(all(load), "over the server's ceiling"));
    CHECK_EQ(s.headerTimeout.count(), 5000);
    CHECK_EQ(s.keepAlive.count(), 30000);
    CHECK_EQ(s.minUploadBytesPerSecond, 512u);
    CHECK_EQ(s.streamsPerAddress, 4u);
    CHECK_EQ(s.requestWait.count(), 2000);
    CHECK_EQ(load.config->logging.keepDays, 90);

    // A server ceiling on connections applies even when the app sets none.
    f.write("app.ms", "config = {}\n");
    load = f.load(ConfigMode::Serve, "server = {\"ceilings\": {\"server\": {\"connections\": 300}}}\n");
    REQUIRE(load.ok());
    CHECK_EQ(load.config->server.connections, 300u);
}

namespace {

FrontConfigLoad loadFront(Fixture& f, const std::string& text) {
    f.dir.write("server.ms", text);
    return loadFrontConfig(ConfigDir::readServerConfig(f.dir.at("server.ms")).value(), *f.host);
}

std::string all(const FrontConfigLoad& load) {
    std::string s;
    for (const Diagnostic& d : load.diagnostics) s += d.toString() + "\n";
    return s;
}

}  // namespace

TEST_CASE("front config: server.apps and server.front") {
    Fixture f;
    FrontConfigLoad ok = loadFront(f, R"(server = {}
server.apps = {}
server.apps.club = {"domains": ["Club.Example.com", "www.club.example.com"]}
server.apps["shop-2"] = {"domains": ["shop.example.com"], "socket": "/run/x/shop.sock",
    "certFile": "/etc/fourfold/shop.crt", "keyFile": "/etc/fourfold/shop.key"}
server.front = {"tls": {"email": "admin@example.com", "acme": "letsencrypt-staging", "http2": false},
    "server": {"connectionsPerAddress": 50, "headerSeconds": 5}}
server.ceilings = {"budget": {"seconds": 20}}
)");
    REQUIRE_MESSAGE(ok.config, all(ok));
    const FrontConfig& c = *ok.config;
    REQUIRE_EQ(c.apps.size(), 2u);
    CHECK_EQ(c.apps[0].name, "club");
    CHECK_EQ(c.apps[0].domains, std::vector<std::string>{"club.example.com", "www.club.example.com"});
    CHECK_EQ(c.apps[0].socket, "/run/fourfold/club.sock");
    CHECK(c.apps[0].certFile.empty());
    CHECK_EQ(c.apps[1].name, "shop-2");
    CHECK_EQ(c.apps[1].socket, "/run/x/shop.sock");
    CHECK_EQ(c.apps[1].keyFile, "/etc/fourfold/shop.key");
    CHECK_EQ(c.tls.email, "admin@example.com");
    CHECK(c.tls.acmeDirectory.find("staging") != std::string::npos);
    CHECK_FALSE(c.tls.http2);
    CHECK_EQ(c.server.connectionsPerAddress, 50u);
    CHECK_EQ(c.server.headerTimeout.count(), 5000);

    // The same file still serves the apps' engines as their ceilings.
    f.write("app.ms", "config = {}\nconfig.budget = {\"seconds\": 30}\n");
    ConfigLoad app = f.load(ConfigMode::Serve, R"(server = {}
server.apps = {}
server.apps.club = {"domains": ["club.example.com"]}
server.front = {}
server.ceilings = {"budget": {"seconds": 20}}
)");
    REQUIRE_MESSAGE(app.ok(), all(app));
    CHECK_EQ(app.config->budget.wallClock.count(), 20000);
}

TEST_CASE("front config: mistakes are reported") {
    Fixture f;
    auto errors = [&](const std::string& text) {
        FrontConfigLoad load = loadFront(f, text);
        CHECK_FALSE(load.config);
        return all(load);
    };
    CHECK(contains(errors("server = {}\n"), "server.apps must list the apps"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"Bad Name\": {\"domains\": [\"a.com\"]}}\n"),
                   "an app's name must be lowercase"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"a\": {\"domains\": [\"x.com\"]}, \"b\": {\"domains\": [\"X.com\"]}}\n"),
                   "which server.apps.a has too"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"a\": {\"domains\": [\"https://x.com\"]}}\n"),
                   "isn't a host name"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"a\": {}}\n"), "server.apps.a.domains is missing"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"a\": {\"domains\": [\"x.com\"], \"socket\": \"rel.sock\"}}\n"),
                   "must be a full path"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"a\": {\"domains\": [\"x.com\"], \"certFile\": \"/c\"}}\n"),
                   "needs its partner"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"a\": {\"domains\": [\"x.com\"], \"sockett\": \"/x\"}}\n"),
                   "unknown setting server.apps.a.sockett"));
    CHECK(contains(errors("server = {}\nserver.apps = {\"a\": {\"domains\": [\"x.com\"]}}\nserver.front = {\"tls\": {\"certFile\": \"c.pem\", \"keyFile\": \"k.pem\"}}\n"),
                   "go in each app's entry"));
}

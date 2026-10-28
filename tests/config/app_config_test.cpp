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

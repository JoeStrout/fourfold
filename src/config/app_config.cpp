#include "config/app_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace fourfold {

namespace {

const char* const kAppFile = "app.ms";
const char* const kSecretsFile = "secrets.ms";

class Problems {
public:
    void error(const std::string& file, const std::string& message, int line = 0) {
        all.push_back({Diagnostic::Severity::Error, file, line, message});
        errors++;
    }
    void warning(const std::string& file, const std::string& message) {
        all.push_back({Diagnostic::Severity::Warning, file, 0, message});
    }

    std::vector<Diagnostic> all;
    int errors = 0;
};

// One map of settings ("config.auth"), read a key at a time. Keys that are
// never read are reported as unknown by finish().
class Section {
public:
    Section(const Datum& map, std::string name, std::string file, Problems& problems)
        : name_(std::move(name)), file_(std::move(file)), problems_(problems) {
        if (map.kind() == Datum::Kind::Map) map_ = map.asMap();
    }

    const std::string& file() const { return file_; }
    std::string where(const std::string& key) const { return name_ + "." + key; }
    bool has(const std::string& key) const { return map_.count(key) > 0; }

    void error(const std::string& key, const std::string& message) { problems_.error(file_, where(key) + " " + message); }

    std::optional<Datum> raw(const std::string& key) {
        read_.insert(key);
        auto it = map_.find(key);
        if (it == map_.end() || it->second.isNull()) return std::nullopt;
        return it->second;
    }

    std::optional<double> number(const std::string& key, double min, double max) {
        auto v = raw(key);
        if (!v) return std::nullopt;
        if (v->kind() != Datum::Kind::Number || !(v->asNumber() >= min && v->asNumber() <= max)) {
            error(key, "must be a number from " + show(min) + " to " + show(max));
            return std::nullopt;
        }
        return v->asNumber();
    }

    std::optional<std::string> string(const std::string& key) {
        auto v = raw(key);
        if (!v) return std::nullopt;
        if (v->kind() != Datum::Kind::String) {
            error(key, "must be a string");
            return std::nullopt;
        }
        return v->asString();
    }

    // MiniScript's true and false are 1 and 0.
    std::optional<bool> boolean(const std::string& key) {
        auto v = raw(key);
        if (!v) return std::nullopt;
        if (v->kind() != Datum::Kind::Number || (v->asNumber() != 0 && v->asNumber() != 1)) {
            error(key, "must be true or false");
            return std::nullopt;
        }
        return v->asNumber() == 1;
    }

    std::optional<std::vector<std::string>> strings(const std::string& key) {
        auto v = raw(key);
        if (!v) return std::nullopt;
        std::vector<std::string> out;
        bool ok = v->kind() == Datum::Kind::List;
        if (ok) {
            for (const Datum& item : v->asList()) {
                ok = ok && item.kind() == Datum::Kind::String;
                if (ok) out.push_back(item.asString());
            }
        }
        if (!ok) {
            error(key, "must be a list of strings");
            return std::nullopt;
        }
        return out;
    }

    Section section(const std::string& key) {
        auto v = raw(key);
        if (v && v->kind() != Datum::Kind::Map) error(key, "must be a map, like {\"name\": value}");
        return Section(v ? *v : Datum(), where(key), file_, problems_);
    }

    void finish() {
        for (const auto& [key, value] : map_) {
            if (!read_.count(key)) problems_.error(file_, "unknown setting " + where(key));
        }
    }

    static std::string show(double n) {
        if (n == std::floor(n) && std::fabs(n) < 1e15) return std::to_string(static_cast<long long>(n));
        return std::to_string(n);
    }

private:
    std::map<std::string, Datum> map_;
    std::set<std::string> read_;
    std::string name_;
    std::string file_;
    Problems& problems_;
};

// The server's ceilings, by "budget.instructions" and so on.
using Ceilings = std::map<std::string, double>;

// A number setting: the app's value if set, else `fallback`, then lowered to
// the server's ceiling if it's over.
double limited(Section& s, const std::string& group, const std::string& key, double fallback, double min, double max,
               const Ceilings& ceilings, Problems& problems) {
    std::optional<double> v = s.number(key, min, max);
    double value = v.value_or(fallback);
    auto c = ceilings.find(group + "." + key);
    if (c != ceilings.end() && value > c->second) {
        if (v) {
            problems.warning(s.file(), s.where(key) + " is " + Section::show(value) +
                                           ", over the server's ceiling of " + Section::show(c->second) +
                                           "; using the ceiling");
        }
        value = c->second;
    }
    return value;
}

bool isHostName(const std::string& s) {
    if (s.empty() || s.size() > 253 || s.front() == '.' || s.back() == '.' || s.front() == '-') return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.')) return false;
    }
    return true;
}

bool isLocalHost(const std::string& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

// Evaluates a config file for its one global, reporting problems.
std::optional<Datum> evaluateFile(const ConfigFile& file, const std::string& global, DataEvaluator& evaluator,
                                  Problems& problems) {
    EvaluatedData d = evaluator.evaluate(file, global);
    if (d.error) {
        problems.error(d.error->file.empty() ? file.name() : d.error->file, d.error->message, d.error->line);
        return std::nullopt;
    }
    if (d.value.kind() != Datum::Kind::Map) {
        problems.error(file.name(), d.value.isNull() ? "must set `" + global + "`, a map (start with " + global +
                                                           " = {})"
                                                     : "`" + global + "` must be a map");
        return std::nullopt;
    }
    return d.value;
}

Ceilings readServer(const Datum& server, const std::string& file, Problems& problems) {
    Ceilings out;
    Section top(server, "server", file, problems);
    Section ceilings = top.section("ceilings");
    for (const char* group : {"budget", "session"}) {
        Section g = ceilings.section(group);
        const std::vector<const char*> keys =
            std::string(group) == "budget"
                ? std::vector<const char*>{"instructions", "seconds"}
                : std::vector<const char*>{"capKB", "anonIdleHours", "anonMaxDays", "userIdleDays", "userMaxDays",
                                           "anonPerAddress", "anonTotal"};
        for (const char* key : keys) {
            if (auto v = g.number(key, 0, 1e12)) out[std::string(group) + "." + key] = *v;
        }
        g.finish();
    }
    ceilings.finish();
    top.finish();
    return out;
}

std::map<std::string, Secret> readSecrets(const Datum& secrets, const std::string& file, Problems& problems) {
    std::map<std::string, Secret> out;
    for (const auto& [name, value] : secrets.asMap()) {
        if (!isIdentifier(name)) {
            problems.error(file, "secrets." + name + ": a secret's name must be a valid identifier");
            continue;
        }
        Section s(value, "secrets." + name, file, problems);
        if (value.kind() != Datum::Kind::Map) {
            problems.error(file, "secrets." + name + " must be a map, like {\"value\": \"...\"}");
            continue;
        }
        Secret secret;
        auto v = s.string("value");
        if (!v) {
            if (!s.has("value")) s.error("value", "is missing");
            s.finish();
            continue;
        }
        secret.value = *v;
        if (auto hosts = s.strings("hosts")) {
            for (const std::string& h : *hosts) {
                std::string host = lowered(h);
                bool subdomains = host.compare(0, 2, "*.") == 0;
                if (!isValidHostName(subdomains ? host.substr(2) : host) ||
                    (subdomains && host.find('.', 2) == std::string::npos)) {
                    s.error("hosts", "has \"" + h + "\", which isn't a host name (or \"*.example.com\" for its "
                                     "subdomains)");
                } else {
                    secret.hosts.push_back(host);
                }
            }
        }
        if (auto verify = s.boolean("verify")) secret.verify = *verify;
        s.finish();
        out.emplace(name, std::move(secret));
    }
    return out;
}

// An engine-only secret's value, named by setting `key`.
std::optional<std::string> engineSecret(Section& s, const std::string& key, const std::map<std::string, Secret>& secrets) {
    auto name = s.string(key);
    if (!name) return std::nullopt;
    auto it = secrets.find(*name);
    if (it == secrets.end()) {
        s.error(key, "names the secret \"" + *name + "\", which isn't in private/config/secrets.ms");
        return std::nullopt;
    }
    if (!it->second.engineOnly()) {
        s.error(key, "names the secret \"" + *name + "\", which has \"" +
                         (it->second.hosts.empty() ? "verify" : "hosts") +
                         "\" and so is visible to app code; an engine secret like this one must have neither");
        return std::nullopt;
    }
    return it->second.value;
}

void readAuth(Section auth, AppConfig& c) {
    if (auto methods = auth.strings("methods")) {
        c.auth.magicLink = false;
        for (const std::string& m : *methods) {
            if (m == "magicLink") {
                c.auth.magicLink = true;
            } else {
                auth.error("methods", "has \"" + m + "\"; the only sign-in method so far is \"magicLink\"");
            }
        }
    }
    if (auto admins = auth.strings("admins")) {
        for (const std::string& a : *admins) {
            std::string email = AuthStore::normalizeEmail(a);
            if (!isValidEmailAddress(email)) {
                auth.error("admins", "has \"" + a + "\", which isn't an email address");
            } else {
                c.auth.admins.push_back(email);
            }
        }
    }
    if (auto signUp = auth.string("signUp")) {
        if (*signUp == "open" || *signUp == "closed") {
            c.auth.openSignUp = *signUp == "open";
        } else {
            auth.error("signUp", "must be \"open\" or \"closed\"");
        }
    }
    if (auto fake = auth.boolean("fakeLogin")) {
        if (*fake && c.mode == ConfigMode::Serve) {
            auth.error("fakeLogin", "can't be on with `fourfold serve`: anyone could sign in as anyone");
        } else {
            c.auth.fakeLogin = *fake;
        }
    }
    if (auto minutes = auth.number("linkMinutes", 1, 24 * 60)) {
        c.auth.store.tokenLifetime = std::chrono::milliseconds(static_cast<int64_t>(*minutes * 60'000));
    }
    auth.finish();
}

void readMail(Section mail, AppConfig& c, Problems& problems) {
    std::optional<std::string> transport = mail.string("transport");
    if (transport) {
        static const std::map<std::string, MailConfig::Transport> kTransports = {
            {"none", MailConfig::Transport::None},
            {"log", MailConfig::Transport::Log},
            {"smtp", MailConfig::Transport::Smtp},
            {"postmark", MailConfig::Transport::Postmark}};
        auto it = kTransports.find(*transport);
        if (it == kTransports.end()) {
            mail.error("transport", "must be \"smtp\", \"postmark\", \"log\" or \"none\"");
        } else {
            c.mail.transport = it->second;
        }
    }
    if (auto from = mail.string("from")) c.mail.from = *from;
    bool smtp = c.mail.transport == MailConfig::Transport::Smtp;
    bool postmark = c.mail.transport == MailConfig::Transport::Postmark;

    SmtpSettings s;
    if (auto host = mail.string("smtpHost")) s.host = lowered(*host);
    if (auto port = mail.number("smtpPort", 1, 65535)) s.port = static_cast<int>(*port);
    std::optional<std::string> security = mail.string("smtpSecurity");
    if (security) {
        if (*security == "starttls") {
            s.security = SmtpSettings::Security::StartTls;
        } else if (*security == "tls") {
            s.security = SmtpSettings::Security::Tls;
            if (!mail.has("smtpPort")) s.port = 465;
        } else if (*security == "none") {
            s.security = SmtpSettings::Security::None;
        } else {
            mail.error("smtpSecurity", "must be \"starttls\", \"tls\" or \"none\"");
        }
    }
    if (auto user = mail.string("smtpUser")) s.username = *user;
    std::optional<std::string> password = engineSecret(mail, "passwordSecret", c.secrets);

    PostmarkSettings p;
    std::optional<std::string> token = engineSecret(mail, "postmarkTokenSecret", c.secrets);
    if (auto stream = mail.string("postmarkStream")) p.messageStream = *stream;
    if (auto appMail = mail.boolean("appMail")) c.mail.appMail = *appMail;
    if (auto perHour = mail.number("appPerHour", 1, 1e6)) c.mail.appPerHour = static_cast<int>(*perHour);
    mail.finish();

    auto before = problems.errors;
    if ((smtp || postmark) && c.mail.from.empty()) {
        mail.error("from", "is needed to send mail (\"noreply@example.com\", or \"Name <noreply@example.com>\")");
    } else if (smtp || postmark) {
        MailMessage probe{c.mail.from, "probe@example.com", "", "", std::nullopt};
        if (!validateMessage(probe)) mail.error("from", "isn't a valid sender address");
    }
    if (smtp) {
        if (s.host.empty() || !isHostName(s.host)) mail.error("smtpHost", "must be the SMTP server's host name");
        if (s.security == SmtpSettings::Security::None && !isLocalHost(s.host)) {
            mail.error("smtpSecurity", "can be \"none\" only for a server on this machine (localhost); mail to "
                                       "another machine must use TLS");
        }
        if (!s.username.empty() && !password) mail.error("passwordSecret", "is needed with smtpUser");
        if (password) s.password = *password;
    }
    if (postmark && !token) mail.error("postmarkTokenSecret", "is needed: the secret holding the server token");
    if (problems.errors != before) return;

    switch (c.mail.transport) {
        case MailConfig::Transport::None: break;
        case MailConfig::Transport::Log: break;   // made by loadConfig, with the dev output
        case MailConfig::Transport::Smtp: c.mailer = makeSmtpMailer(s); break;
        case MailConfig::Transport::Postmark:
            p.serverToken = *token;
            c.mailer = makePostmarkMailer(p);
            break;
    }
}

void readSession(Section session, AppConfig& c, const Ceilings& ceilings, Problems& problems) {
    using namespace std::chrono;
    SessionConfig& s = c.session;
    auto hours = [](double h) { return milliseconds(static_cast<int64_t>(h * 3600'000)); };
    auto get = [&](const char* key, double fallback, double min, double max) {
        return limited(session, "session", key, fallback, min, max, ceilings, problems);
    };
    s.maxBytes = static_cast<size_t>(get("capKB", s.maxBytes / 1024.0, 1, 16 * 1024) * 1024);
    s.anonymousIdle = hours(get("anonIdleHours", duration<double, std::ratio<3600>>(s.anonymousIdle).count(), 0.1, 24 * 365));
    s.anonymousLimit = hours(24 * get("anonMaxDays", duration<double, std::ratio<86400>>(s.anonymousLimit).count(), 0.01, 3650));
    s.signedInIdle = hours(24 * get("userIdleDays", duration<double, std::ratio<86400>>(s.signedInIdle).count(), 0.01, 3650));
    s.signedInLimit = hours(24 * get("userMaxDays", duration<double, std::ratio<86400>>(s.signedInLimit).count(), 0.01, 3650));
    s.maxAnonymousPerAddress = static_cast<size_t>(get("anonPerAddress", static_cast<double>(s.maxAnonymousPerAddress), 1, 1e9));
    s.maxAnonymous = static_cast<size_t>(get("anonTotal", static_cast<double>(s.maxAnonymous), 1, 1e10));
    session.finish();
}

void readBudget(Section budget, AppConfig& c, const Ceilings& ceilings, Problems& problems) {
    ScriptBudget& b = c.budget;
    b.instructions = static_cast<uint64_t>(
        limited(budget, "budget", "instructions", static_cast<double>(b.instructions), 1000, 1e12, ceilings, problems));
    double seconds = limited(budget, "budget", "seconds", b.wallClock.count() / 1000.0, 0.1, 3600, ceilings, problems);
    b.wallClock = std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
    if (budget.has("memoryMB")) {
        budget.raw("memoryMB");
        budget.error("memoryMB", "isn't supported yet: there's no per-request memory limit in this version");
    }
    budget.finish();
}

// The engine's default rates, as written: one string or a list.
std::optional<std::vector<RateRule>> readRates(Section& s, const std::string& key, RateRule::Per per) {
    auto v = s.raw(key);
    if (!v) return std::nullopt;
    std::vector<Datum> items;
    if (v->kind() == Datum::Kind::String) items.push_back(*v);
    if (v->kind() == Datum::Kind::List) items = v->asList();
    if (items.empty()) {
        s.error(key, "must be a rate like \"60/minute\", or a list of them");
        return std::nullopt;
    }
    std::vector<RateRule> out;
    for (const Datum& item : items) {
        std::string error;
        std::optional<RateRule> r = item.kind() == Datum::Kind::String ? parseRate(item.asString(), error)
                                                                       : std::nullopt;
        if (!r) {
            s.error(key, error.empty() ? "must be a rate like \"60/minute\", or a list of them" : error);
            return std::nullopt;
        }
        if (item.asString().find("per ip") != std::string::npos ||
            item.asString().find("per user") != std::string::npos ||
            item.asString().find("per session") != std::string::npos ||
            std::count(item.asString().begin(), item.asString().end(), '/') > 1) {
            s.error(key, "counts " + std::string(per == RateRule::Per::Ip ? "per address" : "per user") +
                             " already; write just the rate, like \"60/minute\"");
            return std::nullopt;
        }
        r->per = per;
        r->key = "default";
        out.push_back(*r);
    }
    return out;
}

void readLimits(Section limits, AppConfig& c) {
    if (auto rules = readRates(limits, "anonymous", RateRule::Per::Ip)) c.limits.anonymous = *rules;
    if (auto rules = readRates(limits, "signedIn", RateRule::Per::User)) c.limits.signedIn = *rules;
    if (auto bits = limits.number("challengeBits", 1, 32)) c.limits.challengeBits = static_cast<int>(*bits);
    limits.finish();
}

void readProxy(Section proxy, AppConfig& c) {
    if (auto trust = proxy.strings("trust")) {
        for (const std::string& t : *trust) {
            auto range = AddressRange::parse(t);
            if (!range) {
                proxy.error("trust", "has \"" + t + "\", which isn't an IP address or a range like \"10.0.0.0/8\"");
            } else {
                c.proxy.trust.push_back(*range);
            }
        }
    }
    proxy.finish();
}

// config.outbound (overview §6): only ever tightens code/outbound.ms.
void readOutbound(Section outbound, AppConfig& c) {
    OutboundSettings& o = c.outbound;
    if (auto enabled = outbound.boolean("enabled")) o.enabled = *enabled;
    if (auto allow = outbound.strings("allow")) {
        std::vector<HostRule> cap;
        for (const std::string& a : *allow) {
            std::string error;
            if (auto rule = parseHostRule(a, error)) {
                cap.push_back(*rule);
            } else {
                outbound.error("allow", error);
            }
        }
        o.cap = std::move(cap);
    }
    if (auto addresses = outbound.strings("allowAddresses")) {
        for (const std::string& a : *addresses) {
            auto range = AddressRange::parse(a);
            if (!range) {
                outbound.error("allowAddresses", "has \"" + a + "\", which isn't an IP address or a range like "
                                                 "\"10.0.0.0/24\"");
            } else {
                o.allowAddresses.push_back(*range);
            }
        }
    }
    if (auto perMinute = outbound.number("perMinute", 1, 100000)) o.perMinute = static_cast<int>(*perMinute);
    if (auto kb = outbound.number("maxResponseKB", 1, 1024 * 1024)) o.maxResponseBytes = static_cast<size_t>(*kb * 1024);
    if (auto seconds = outbound.number("timeoutSeconds", 0.1, 600)) {
        o.timeout = std::chrono::milliseconds(static_cast<int64_t>(*seconds * 1000));
    }
    outbound.finish();
}

void readDatabase(Section db, AppConfig& c) {
    bool timeoutSet = false;
    if (auto url = db.string("url")) {
        std::string u = *url;
        if (u.rfind("postgres://", 0) == 0 || u.rfind("postgresql://", 0) == 0) {
            c.db.dialect = SqlDialect::Postgres;
            c.db.postgres.url = u;
            if (u.find('@') != std::string::npos &&
                u.substr(0, u.find('@')).find(':', u.find("://") + 3) != std::string::npos) {
                db.error("url", "has a password in it; put the password in secrets.ms, and name that secret with "
                                "passwordSecret");
            }
            if (!Database::postgresSupported()) {
                db.error("url", "names a Postgres database, but this build of Fourfold has no Postgres support "
                                "(it was built without libpq)");
            }
        } else {
            db.error("url", "must be a Postgres URL, like \"postgres://db.internal/myapp\" (leave config.db out to "
                            "use SQLite)");
        }
    }
    std::optional<std::string> password = engineSecret(db, "passwordSecret", c.secrets);
    if (password) c.db.postgres.password = *password;
    if (auto schema = db.string("schema")) {
        if (!isIdentifier(*schema)) {
            db.error("schema", "must be a schema's name (letters, digits and _)");
        } else {
            c.db.postgres.schema = *schema;
        }
    }
    if (auto pool = db.number("pool", 1, 100)) c.db.poolSize = static_cast<size_t>(*pool);
    if (auto seconds = db.number("queryTimeoutSeconds", 0.1, 3600)) {
        c.db.limits.queryTimeout = std::chrono::milliseconds(static_cast<int64_t>(*seconds * 1000));
        timeoutSet = true;
    }
    if (auto rows = db.number("maxRows", 1, 1e7)) c.db.limits.maxRows = static_cast<size_t>(*rows);
    if (c.db.dialect == SqlDialect::Sqlite && (password || c.db.postgres.schema.size())) {
        db.error(password ? "passwordSecret" : "schema", "is for a Postgres database (set url too)");
    }
    db.finish();
    // A query may take as long as the request that runs it.
    if (!timeoutSet) c.db.limits.queryTimeout = c.budget.wallClock;
}

void readDeploy(Section deploy, AppConfig& c) {
    if (auto s = deploy.number("migrationWaitSeconds", 0, 3600)) {
        c.deploy.migrationWait = std::chrono::milliseconds(static_cast<int64_t>(*s * 1000));
    }
    if (auto s = deploy.number("migrationBudgetSeconds", 1, 7 * 24 * 3600)) {
        c.deploy.migrationBudget = std::chrono::milliseconds(static_cast<int64_t>(*s * 1000));
    }
    if (auto clear = deploy.boolean("clearAnonymousSessions")) c.deploy.clearAnonymousSessions = *clear;
    deploy.finish();
}

void readBackups(Section backups, AppConfig& c) {
    if (auto folder = backups.string("folder")) {
        if (folder->empty() || (*folder)[0] != '/') {
            backups.error("folder", "must be a full path, like \"/var/backups/myapp\", outside the app's folders");
        } else {
            c.backups.folder = *folder;
        }
    }
    if (auto hours = backups.number("everyHours", 0, 24 * 365)) c.backups.everyHours = *hours;
    if (auto keep = backups.number("keep", 1, 10000)) c.backups.keep = static_cast<size_t>(*keep);
    backups.finish();
}

}  // namespace

ConfigLoad loadConfig(const ConfigSources& sources, DataEvaluator& evaluator) {
    Problems problems;
    auto c = std::make_shared<AppConfig>();
    c->mode = sources.mode;
    std::string unused;
    c->limits.anonymous = {*parseRate("60/minute", unused)};
    c->limits.signedIn = {*parseRate("300/minute per user", unused)};
    for (RateRule* r : {&c->limits.anonymous[0], &c->limits.signedIn[0]}) r->key = "default";
    if (sources.mode == ConfigMode::Dev) {
        c->auth.fakeLogin = true;
        c->mail.transport = MailConfig::Transport::Log;
        c->limits.challengeBits = 4;
    }

    Ceilings ceilings;
    if (sources.serverConfig) {
        if (auto server = evaluateFile(*sources.serverConfig, "server", evaluator, problems)) {
            ceilings = readServer(*server, sources.serverConfig->name(), problems);
        }
    }

    Datum app = Datum::map({});
    std::string appName = "private/config/app.ms";
    if (sources.dir) {
        auto secretsFile = sources.dir->read(kSecretsFile);
        if (!secretsFile) {
            problems.error("private/config/secrets.ms", secretsFile.error().message);
        } else if (secretsFile.value()) {
            if (auto secrets = evaluateFile(*secretsFile.value(), "secrets", evaluator, problems)) {
                c->secrets = readSecrets(*secrets, secretsFile.value()->name(), problems);
            }
        }
        auto appFile = sources.dir->read(kAppFile);
        if (!appFile) {
            problems.error(appName, appFile.error().message);
        } else if (appFile.value()) {
            if (auto read = evaluateFile(*appFile.value(), "config", evaluator, problems)) app = *read;
        }
    }

    Section top(app, "config", appName, problems);
    if (auto domain = top.string("domain")) {
        if (!isHostName(lowered(*domain))) {
            top.error("domain", "must be a host name like \"example.com\" (no https://, port or path)");
        } else {
            c->site.domain = lowered(*domain);
        }
    }
    readBudget(top.section("budget"), *c, ceilings, problems);
    readSession(top.section("session"), *c, ceilings, problems);
    readAuth(top.section("auth"), *c);
    readMail(top.section("mail"), *c, problems);
    readLimits(top.section("limits"), *c);
    readProxy(top.section("proxy"), *c);
    readOutbound(top.section("outbound"), *c);
    readDatabase(top.section("db"), *c);
    readDeploy(top.section("deploy"), *c);
    readBackups(top.section("backups"), *c);
    top.finish();

    if (c->mail.transport == MailConfig::Transport::Log) {
        c->mailer = makeLogMailer(sources.devMailOutput);
        if (c->mail.from.empty()) c->mail.from = "noreply@localhost.invalid";
    }
    if (c->mode == ConfigMode::Serve && c->auth.magicLink && c->mail.transport != MailConfig::Transport::None &&
        c->site.domain.empty()) {
        problems.error(appName, "config.domain is needed to send sign-in links (they point to the site)");
    }

    ConfigLoad out;
    out.diagnostics = std::move(problems.all);
    if (problems.errors == 0) out.config = std::move(c);
    return out;
}

std::string configStamp(const ConfigDir& dir) {
    return dir.stamp(kAppFile) + "|" + dir.stamp(kSecretsFile);
}

}  // namespace fourfold

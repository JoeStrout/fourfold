// AppConfig: the app's settings (overview §10; Arch §5 config).
//
// Read from private/config/app.ms and secrets.ms, and from the optional
// server config, each MiniScript evaluated as data. Every setting has a
// default, so an app needs no config at all; dev mode's defaults differ
// (fake login on, mail printed instead of sent). App values must fall within
// the server config's ceilings: a value over one is lowered to it, with a
// warning ("rules can only tighten").
//
// Unknown names are errors, so a typo can't silently leave a setting at its
// default. An invalid config is handled like a failed compile: the engine
// keeps the previous one, and says why.
//
// The result is immutable, one sub-struct per component, each handed only
// its own. Secret values never leave this component except inside the
// Mailer built here, so they never reach the script thread.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "appdata/database.h"
#include "base/time_zone.h"
#include "compiledapp/compiled_app.h"
#include "mail/mailer.h"
#include "outbound/outbound_client.h"
#include "partition/config_dir.h"
#include "platform/address.h"
#include "script/script_host.h"
#include "state/auth_store.h"
#include "state/session_store.h"

namespace fourfold {

enum class ConfigMode {
    Dev,     // `fourfold dev`: localhost, fake login, mail printed
    Serve,   // `fourfold serve`
};

struct SiteConfig {
    // The site's host name ("example.com"): sign-in links point to it, and
    // it's added to the host names the app answers to. Empty: none set.
    std::string domain;
};

struct AuthConfig {
    bool magicLink = true;            // sign-in by emailed link
    bool fakeLogin = false;           // dev mode's "sign in as..." picker; never in serve mode
    std::vector<std::string> admins;  // email addresses (lowercased) given the admin role at sign-in
    bool openSignUp = true;           // anyone may sign in (creating a user); else only existing users and admins
    AuthStoreConfig store;            // link lifetime and throttles
};

struct MailConfig {
    enum class Transport { None, Log, Smtp, Postmark };
    Transport transport = Transport::None;
    std::string from;   // the sender, "Name <noreply@example.com>"
    // App mail (the `mail` module): on unless turned off, and limited to
    // this many messages an hour.
    bool appMail = true;
    int appPerHour = 100;
};

// Rate limits and spam checks (overview §5, Rate limits and spam): the
// engine's own; a route's come from code/limits.ms.
struct LimitsConfig {
    // Counted for every request but GET, HEAD and OPTIONS: per address
    // group for anonymous visitors, per user for signed-in ones. Each rule's
    // key is "default".
    std::vector<RateRule> anonymous;
    std::vector<RateRule> signedIn;
    // A proof-of-work puzzle's difficulty, in leading zero bits: 16 takes
    // well under a second on a phone. Low in dev, so testing isn't slowed.
    int challengeBits = 16;
};

// A reverse proxy in front of the engine (overview §8, Trusting the
// client's address): connections from these addresses may say who the
// visitor is, in X-Forwarded-For. Empty: no one may.
struct ProxyConfig {
    std::vector<AddressRange> trust;
};

// The app's database (overview §3): SQLite in data/ unless config.db names
// a Postgres server. Read once, when the server starts: changing it takes a
// restart (a reload keeps the database in use, with a warning).
struct DatabaseConfig {
    SqlDialect dialect = SqlDialect::Sqlite;
    PostgresSettings postgres;   // with dialect Postgres; the password from an engine-only secret
    size_t poolSize = 4;         // connections
    DbLimits limits;             // queryTimeout follows budget.seconds unless set
};

// Deploying and migrating (overview §3, Applying migrations).
struct DeployConfig {
    // How long requests wait while migrations run, before a 503.
    std::chrono::milliseconds migrationWait{30'000};
    // A migration's own time budget (separate from requests').
    std::chrono::milliseconds migrationBudget{600'000};
    // Sign every anonymous visitor out after migrations ran (their
    // sessions may hold the old shapes).
    bool clearAnonymousSessions = false;
};

// Backups (overview §3, Backups).
struct BackupConfig {
    // Where backups go: a host path outside every partition. Empty:
    // private/state/backups/.
    std::string folder;
    // Scheduled backups, every this many hours; 0: none.
    double everyHours = 0;
    // Automatic backups (before migrations, and scheduled) to keep.
    size_t keep = 10;
};

// Background jobs (overview §2, Background jobs; config.jobs).
struct JobsConfig {
    // Jobs running at once, at most: they share the script thread with
    // requests, so a few long jobs can't crowd them out.
    size_t workers = 2;
    // Each job run's budget (its own, longer than a request's).
    ScriptBudget budget = [] {
        ScriptBudget b;
        b.instructions = 100'000'000;
        b.wallClock = std::chrono::milliseconds(60'000);
        return b;
    }();
    // Attempts before a failing job is dead.
    int maxAttempts = 5;
    // The schedule's time zone (IANA), loaded from the system's zoneinfo.
    std::shared_ptr<const TimeZone> timeZone = TimeZone::utc();
};

// TLS, when Fourfold ends it itself (overview §8, TLS; config.tls): a
// certificate from an ACME CA (Let's Encrypt), or files of the app's own.
// Read once, when the server starts.
struct TlsConfig {
    std::string acmeDirectory;   // the CA's directory URL; Let's Encrypt's by default
    std::string email;           // the ACME account's contact, optional
    std::string acmeCaBundle;    // a test CA's root (Pebble), as a host path; empty: the system's
    // Instead of ACME: a certificate chain and key (PEM), in private/config/.
    std::string certFile, keyFile;
    bool http2 = true;
};

// The headers on every response (overview §8, Security headers by default;
// Content Security Policy; config.headers), ready to send.
struct HeadersConfig {
    // The CSP, in two parts: the response's nonce goes between them.
    std::string cspBeforeNonce;
    std::string cspAfterNonce;
    std::string frameOptions = "DENY";   // empty: none (frameAncestors allows embedding)
    std::string hsts;                    // Strict-Transport-Security, over HTTPS in production
    std::string permissionsPolicy;
};
// The defaults, as an app with no config.headers gets.
HeadersConfig defaultHeaders();
// Where browsers send CSP violation reports.
extern const char* const kCspReportPath;

// The standalone server's connection and request limits (overview §8,
// Connection and request limits; config.server). Read when the server
// starts.
struct ServerConfig {
    size_t connections = 0;                // 0: sized to the open-file limit
    size_t connectionsPerAddress = 100;
    size_t streams = 1000;                 // open server-sent event streams
    size_t streamsPerAddress = 20;
    size_t requests = 64;                  // handled at once; more wait
    std::chrono::milliseconds requestWait{5000};
    std::chrono::milliseconds headerTimeout{10000};
    size_t minUploadBytesPerSecond = 1024;
    std::chrono::milliseconds keepAlive{60000};
};

// The engine's logs in private/state/logs/ (config.logging).
struct LoggingConfig {
    int keepDays = 30;   // older day files are removed
};

struct AppConfig {
    ConfigMode mode = ConfigMode::Dev;
    SiteConfig site;
    ScriptBudget budget;
    SessionConfig session;
    AuthConfig auth;
    MailConfig mail;
    LimitsConfig limits;
    ProxyConfig proxy;
    DatabaseConfig db;
    DeployConfig deploy;
    BackupConfig backups;
    // Outbound HTTP (config.outbound): tightens code/outbound.ms.
    OutboundSettings outbound;
    JobsConfig jobs;
    TlsConfig tls;
    HeadersConfig headers = defaultHeaders();
    ServerConfig server;
    LoggingConfig logging;
    // Sends mail as configured; null if the transport is None. Thread-safe.
    std::shared_ptr<Mailer> mailer;
    // By name. Only `config` and the OutboundClient read the values.
    SecretMap secrets;
};

struct ConfigLoad {
    std::shared_ptr<const AppConfig> config;   // null if there were errors
    std::vector<Diagnostic> diagnostics;       // errors and warnings, by file

    bool ok() const { return config != nullptr; }
};

struct ConfigSources {
    const ConfigDir* dir = nullptr;                 // private/config/; null: defaults only
    std::optional<ConfigFile> serverConfig;         // the server config, if one was given
    ConfigMode mode = ConfigMode::Dev;
    // Where the dev mode's log mailer prints each message.
    std::function<void(const std::string&)> devMailOutput;
};

// Reads the config. Waits on the evaluator (the script thread): don't call
// from it.
ConfigLoad loadConfig(const ConfigSources& sources, DataEvaluator& evaluator);

// The config files' identity, for noticing changes: app.ms and secrets.ms.
std::string configStamp(const ConfigDir& dir);

}  // namespace fourfold

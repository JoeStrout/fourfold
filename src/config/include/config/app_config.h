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

#include "compiledapp/compiled_app.h"
#include "mail/mailer.h"
#include "partition/config_dir.h"
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
};

// The secrets from secrets.ms (overview §10, "Secrets: a separate file").
// A secret with no hosts is engine-only (SMTP passwords and the like); one
// with hosts will be usable by app code, only toward those hosts (M9).
struct Secret {
    std::string value;
    std::vector<std::string> hosts;
};

struct AppConfig {
    ConfigMode mode = ConfigMode::Dev;
    SiteConfig site;
    ScriptBudget budget;
    SessionConfig session;
    AuthConfig auth;
    MailConfig mail;
    // Sends mail as configured; null if the transport is None. Thread-safe.
    std::shared_ptr<Mailer> mailer;
    // By name. Only `config` reads the values.
    std::map<std::string, Secret> secrets;
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

// CgiApp: an app served through CGI or FastCGI, behind a web server that
// isn't Fourfold, such as shared hosting's Apache (overview §8, Hosting
// modes; M10).
//
// It opens the app's services as the standalone server does (the config,
// the engine's stores in private/state/state.db, the app's database, data
// files), and serves each request with the version CodeCache gives it. A
// CGI process makes one, serves one request, and exits; a FastCGI process
// keeps one and serves many, one at a time.
//
// What differs from the standalone server, because other processes serve
// the same app at the same time:
//   - session locks and the schema lock are file locks (every request holds
//     the schema lock shared; SchemaFileLock);
//   - a new version's migrations are applied by the request that compiled
//     it, before it's installed (with a backup first, as `fourfold migrate`
//     does); if they fail, the previous version keeps serving;
//   - there's no push (SSE): /live/ channels aren't served, and `publish`
//     does nothing. Pages poll instead;
//   - jobs are queued, and run by `fourfold run-jobs` from cron;
//   - nothing is enforced by the OS (under suEXEC the engine runs as the
//     developer, who can write code/): partitions are the engine's rule only.
// The engine log says so whenever a new version is installed.
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "appdata/backup.h"
#include "appdata/data_files.h"
#include "appdata/database.h"
#include "authflow/auth_flow.h"
#include "cgi/cgi_request.h"
#include "cgi/code_cache.h"
#include "cgi/schema_lock.h"
#include "config/app_config.h"
#include "partition/app_folder.h"
#include "pipeline/engine.h"
#include "script/script_host.h"
#include "state/auth_store.h"
#include "state/engine_log.h"
#include "state/job_queue.h"
#include "state/lock_service.h"
#include "state/rate_store.h"
#include "state/session_store.h"

namespace fourfold {

struct CgiOptions {
    // The server config (overview §10), by host path; empty for none.
    std::string serverConfig;
    // FastCGI: keep the full compiled version in memory between requests.
    bool keepVersion = false;
    const Clock* clock = &systemClock();
    // For tests: how long code/ must stay unchanged before compiling.
    std::optional<int64_t> quietMillis;
};

class CgiApp {
public:
    // Opens the app at `folder` (its partitions must exist). Fails, with a
    // message for the developer, if the folder, its config or its stores
    // can't be opened.
    static Result<std::unique_ptr<CgiApp>> open(const std::string& folder, CgiOptions options = {});
    ~CgiApp();

    CgiApp(const CgiApp&) = delete;
    CgiApp& operator=(const CgiApp&) = delete;

    // Serves one request. Never fails: problems become error responses,
    // with the details in the engine log.
    HttpResponse handle(const HttpRequest& request, bool https);

    // The full version now (compiling a new one first, if code/ changed),
    // with all of its scripts, for `fourfold run-jobs`.
    Result<CodeLoad> loadFull();

    // The services, for run-jobs.
    ScriptHost& scripts() { return *scripts_; }
    EngineLog& log() { return *log_; }
    JobQueue& jobQueue() { return *jobQueue_; }
    SessionStore& sessions() { return *sessions_; }
    std::shared_ptr<const AppConfig> config() const { return config_; }
    const SchemaFileLock& schemaLock() const { return *schemaLock_; }
    // An engine for `load`'s version (taking its code), as handle() uses.
    std::unique_ptr<Engine> engineFor(CodeLoad& load, bool https);

private:
    CgiApp() = default;

    // Reloads the config if its files changed (FastCGI).
    void checkConfig();
    void makeCache();
    // CodeCacheOptions::beforeInstall: the new version's migrations.
    Status migrate(const std::shared_ptr<const CompiledApp>& app);
    // Logs what loading a version did.
    void report(const CodeLoad& load);

    CgiOptions options_;
    std::string root_;
    std::optional<AppFolder> folder_;
    std::optional<CodeTree> code_;
    std::unique_ptr<StateDir> state_;
    std::unique_ptr<DataDir> dataDir_;
    std::unique_ptr<ConfigDir> configDir_;
    std::unique_ptr<Dropbox> dropbox_;
    std::unique_ptr<DataFiles> data_;
    std::unique_ptr<EngineLog> log_;
    std::unique_ptr<ScriptHost> scripts_;
    ConfigSources sources_;
    std::string configStamp_;
    std::shared_ptr<const AppConfig> config_;
    std::unique_ptr<SessionStore> sessions_;
    std::unique_ptr<AuthStore> auth_;
    std::unique_ptr<RateStore> rates_;
    std::unique_ptr<JobQueue> jobQueue_;
    std::unique_ptr<LockService> sessionLocks_;
    std::unique_ptr<Database> database_;
    std::unique_ptr<BackupService> backups_;
    std::unique_ptr<AuthFlow> authFlow_;
    std::unique_ptr<SchemaFileLock> schemaLock_;
    std::string engineKey_;
    std::unique_ptr<CodeCache> cache_;
    // FastCGI: the version in memory, and its engine.
    std::optional<CodeLoad> kept_;
    std::unique_ptr<Engine> keptEngine_;
    bool keptHttps_ = false;
};

// The line the engine log gets when CGI mode installs a version: what it
// doesn't provide.
extern const char* const kCgiModeNotice;

}  // namespace fourfold

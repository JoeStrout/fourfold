// AppServer: everything `fourfold dev` and `fourfold serve` run, put
// together. The script host, the app's stores, the engine, the app instance
// that recompiles on changes, the dev tools (dev mode only), and the HTTP
// server, on one I/O loop.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "acme/certificate_manager.h"
#include "appdata/backup.h"
#include "appdata/data_files.h"
#include "appdata/database.h"
#include "partition/file_lock.h"
#include "authflow/auth_flow.h"
#include "config/app_config.h"
#include "devtools/dev_tools.h"
#include "httpserver/http_server.h"
#include "jobs/job_runner.h"
#include "push/push_hub.h"
#include "lifecycle/app_instance.h"
#include "observer/observer.h"
#include "pipeline/engine.h"
#include "platform/event_loop.h"
#include "script/script_host.h"
#include "state/auth_store.h"
#include "state/engine_log.h"
#include "state/job_queue.h"
#include "state/lock_service.h"
#include "state/rate_store.h"
#include "state/session_store.h"
#include "tls/server_tls.h"

namespace fourfold::cli {

struct ServerOptions {
    // Dev mode: the dev tools, error details in the browser, recompiling on
    // every saved change, and `print` and request lines on the terminal.
    // Otherwise (`fourfold serve`): generic error pages, recompiling only when
    // code/.deploy is touched, and everything logged to the engine log
    // (print output to dropbox/print.log).
    bool dev = true;
    std::string address = "127.0.0.1";
    uint16_t port = 0;   // 0 picks a free port
    // Listen on this Unix-domain socket instead of address:port: behind
    // fourfold front (or another local proxy), whose connections it trusts
    // as a proxy's. Made 0660, so the socket's folder and group decide who
    // can connect.
    std::string unixSocket;
    // The dropbox writer's socket: dropbox/ belongs to another OS user, and
    // appends go to the writer. Empty: written directly.
    std::string dropboxSocket;
    // Host names the app answers to (the Host header, without the port),
    // besides config.domain. Empty, with no domain: localhost's names.
    std::vector<std::string> hosts;
    // The server config (overview §10), by host path; empty for none.
    std::string serverConfig;
    // Reached over HTTPS (through a proxy that ends TLS): Secure cookies,
    // and https:// as the site's origin. Dev mode is plain HTTP.
    bool https = false;
    // Fourfold ends TLS itself (overview §8, TLS): HTTPS on address:port,
    // with a certificate from config.tls (from Let's Encrypt, or files); and
    // on redirectAddress:redirectPort (if set), plain HTTP that only
    // redirects to HTTPS and answers ACME's challenges.
    bool tls = false;
    std::string redirectAddress;
    uint16_t redirectPort = 80;
    // Sockets from systemd's socket activation, used instead of listening
    // on address:port and on the redirect address. (Shared, so options
    // stay copyable; start() takes them.)
    std::shared_ptr<Fd> listenSocket;
    std::shared_ptr<Fd> redirectSocket;
    LifecycleOptions lifecycle;

    // Listen for requests. Off for the CLI's `migrate` and `backup` when no
    // server is running: everything is opened, the app compiled, and the
    // command works through the same app instance.
    bool listen = true;
    // `fourfold serve --migrate`: apply pending migrations before serving
    // (otherwise serve won't start with any pending).
    bool migrateOnStart = false;
    // The emergency override: serve code older than the data's migrations.
    bool allowOlderCode = false;
    // A failing job's first retry waits this long (then doubles); tests
    // shorten it.
    std::chrono::milliseconds jobRetryDelay{15'000};
};

// Whether a Fourfold server (dev or serve) is running the app at `folder`
// now: it holds the app's server lock.
bool serverRunning(const std::string& folder);

// Takes the app's server lock, if it's free (nullopt if a server holds it).
Result<std::optional<FileLock>> takeServerLock(const StateDir& state);

class AppServer : private Observer {
public:
    // Opens and compiles the app at `folder` (creating missing partitions),
    // and listens. Diagnostics go to `err`. An app that doesn't compile still
    // starts: it serves an error page until the code is fixed. Returns null,
    // once `err` says why, if the folder can't be opened, its stores can't be
    // opened, or the port can't be had.
    static std::unique_ptr<AppServer> start(const std::string& folder, ServerOptions options, std::ostream& out,
                                            std::ostream& err);
    // `fourfold dev`: dev mode on 127.0.0.1:port.
    static std::unique_ptr<AppServer> startDev(const std::string& folder, uint16_t port, std::ostream& out,
                                               std::ostream& err, LifecycleOptions lifecycle = {});
    ~AppServer() override;

    AppServer(const AppServer&) = delete;
    AppServer& operator=(const AppServer&) = delete;

    // Serves until loop().stop().
    EventLoop& loop() { return *loop_; }
    // Graceful shutdown (SIGTERM): stops taking connections, lets requests
    // in flight finish (up to `deadline`), then stops the loop. Call on the
    // loop's thread.
    void shutdown(std::chrono::milliseconds deadline);
    // The port 80 server's port (TLS mode), or 0.
    uint16_t redirectPort() const { return redirect_ ? redirect_->port() : 0; }
    // TLS mode's certificates (for tests).
    CertificateManager* certificates() { return certificates_.get(); }
    ServerTls* tls() { return tls_.get(); }
    uint16_t port() const { return server_->port(); }
    const std::string& root() const { return root_; }
    // Whether the first compile succeeded.
    bool compiled() const { return firstCompileOk_; }
    // The engine log (private/state/logs/).
    EngineLog& log() { return *log_; }
    // For tests: locks held or waited for, and runs in progress.
    LockService::Stats sessionLockStats() const { return sessionLocks_->stats(); }
    LockService::Stats dataLockStats() const { return data_->locks().stats(); }
    // The engine's users (for tests).
    AuthStore& users() { return *auth_; }
    // Rate-limit counters (for tests).
    RateStore& rates() { return *rates_; }
    // The config in use.
    std::shared_ptr<const AppConfig> config() const { return engine_->config(); }
    ScriptHost::Stats scriptStats() { return scripts_->stats(); }
    // The app instance: migrations and backups.
    AppInstance& instance() { return *instance_; }
    // The app's database (opened on first use), or null.
    Database* database() { return database_.get(); }
    BackupService& backups() { return *backups_; }
    SchemaGate::Stats schemaStats() const { return schema_.stats(); }
    // Background jobs (M9.1): the queue, and the runner.
    JobQueue& jobQueue() { return *jobQueue_; }
    JobRunner& jobs() { return *jobRunner_; }
    // Push channels' subscribers.
    PushHub& push() { return *push_; }

private:
    AppServer(std::ostream& out, std::ostream& err) : out_(out), err_(err) {}

    // Output comes from the I/O loop, the script thread and the lifecycle
    // thread; a line at a time.
    void say(const std::string& line);
    void sayError(const std::string& line);

    // Observer: the terminal's view of compiles and script errors (and in
    // serve mode, the engine log's).
    void compileFinished(const CompileReport& report) override;
    void scriptError(const ScriptError& e) override;
    void renderWarnings(uint64_t generation, const std::vector<RenderWarning>& warnings) override;
    void jobsEnqueued(const std::string& source, const std::vector<std::string>& names, bool committed) override;
    void cspViolation(const std::string& description) override;

    // Removes expired sessions and sign-in tokens now, and every hour.
    void scheduleSweep(std::chrono::milliseconds delay);
    // Reports a config's problems; puts a valid one into the stores and the
    // script host (the engine has it already).
    void configLoaded(const ConfigLoad& load, bool first);
    // Where dev mode's printed mail goes.
    void printMail(const std::string& text);
    // Requests from `fourfold migrate` and `fourfold backup` (files in
    // private/state/control/): checked now, and every so often.
    void scheduleControl(std::chrono::milliseconds delay);
    void checkControl();
    // TLS mode: the certificates (files, or ACME), and port 80's server.
    Status startTls(const AppConfig& config, const std::vector<std::string>& hosts, const ServerOptions& options);
    // Scheduled backups (config.backups.everyHours).
    void scheduleBackup(std::chrono::milliseconds delay);

    std::ostream& out_;
    std::ostream& err_;
    std::mutex outMutex_;
    std::string root_;
    bool dev_ = true;
    bool started_ = false;   // the first compile has been reported
    bool firstCompileOk_ = false;
    // Render warnings already printed, for the generation they came from:
    // each is printed once, not on every request.
    uint64_t warnedGeneration_ = 0;
    std::set<std::string> warned_;   // guarded by outMutex_

    // In order of construction. Destroyed in a different order (see the
    // destructor): whatever posts to or calls into something must stop first.
    std::unique_ptr<EventLoop> loop_;
    std::optional<FileLock> serverLock_;   // held while this server runs the app
    std::unique_ptr<StateDir> state_;
    std::unique_ptr<DataDir> dataDir_;     // for migrations and backups
    std::unique_ptr<Database> database_;
    std::string dbUrl_, dbSchema_;         // the database it started with
    std::unique_ptr<BackupService> backups_;
    SchemaGate schema_;
    DataServices dataServices_;
    std::thread controlThread_;            // a control request being carried out
    std::atomic<bool> controlBusy_{false};
    std::unique_ptr<EngineLog> log_;
    std::unique_ptr<Dropbox> dropbox_;
    std::unique_ptr<ScriptHost> scripts_;
    std::unique_ptr<ConfigDir> configDir_;
    std::unique_ptr<SessionStore> sessions_;
    std::unique_ptr<AuthStore> auth_;
    std::unique_ptr<RateStore> rates_;
    std::unique_ptr<JobQueue> jobQueue_;
    std::shared_ptr<PushHub> push_;
    std::unique_ptr<LockService> sessionLocks_;
    std::unique_ptr<DataFiles> data_;
    std::unique_ptr<AuthFlow> authFlow_;
    std::unique_ptr<DevTools> devtools_;
    ObserverList observers_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<AppInstance> instance_;
    std::unique_ptr<JobRunner> jobRunner_;
    std::shared_ptr<ServerTls> tls_;
    std::unique_ptr<CertificateManager> certificates_;
    std::unique_ptr<HttpServer> redirect_;   // port 80, in TLS mode
    std::unique_ptr<HttpServer> server_;
};

}  // namespace fourfold::cli

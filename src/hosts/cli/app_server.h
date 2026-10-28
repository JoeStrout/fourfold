// AppServer: everything `fourfold dev` and `fourfold serve` run, put
// together. The script host, the app's stores, the engine, the app instance
// that recompiles on changes, the dev tools (dev mode only), and the HTTP
// server, on one I/O loop.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "appdata/data_files.h"
#include "authflow/auth_flow.h"
#include "config/app_config.h"
#include "devtools/dev_tools.h"
#include "httpserver/http_server.h"
#include "lifecycle/app_instance.h"
#include "observer/observer.h"
#include "pipeline/engine.h"
#include "platform/event_loop.h"
#include "script/script_host.h"
#include "state/auth_store.h"
#include "state/engine_log.h"
#include "state/lock_service.h"
#include "state/session_store.h"

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
    // Host names the app answers to (the Host header, without the port),
    // besides config.domain. Empty, with no domain: localhost's names.
    std::vector<std::string> hosts;
    // The server config (overview §10), by host path; empty for none.
    std::string serverConfig;
    // Reached over HTTPS (through a proxy that ends TLS): Secure cookies,
    // and https:// as the site's origin. Dev mode is plain HTTP.
    bool https = false;
    LifecycleOptions lifecycle;
};

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
    // The config in use.
    std::shared_ptr<const AppConfig> config() const { return engine_->config(); }
    ScriptHost::Stats scriptStats() { return scripts_->stats(); }

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

    // Removes expired sessions and sign-in tokens now, and every hour.
    void scheduleSweep(std::chrono::milliseconds delay);
    // Reports a config's problems; puts a valid one into the stores and the
    // script host (the engine has it already).
    void configLoaded(const ConfigLoad& load, bool first);
    // Where dev mode's printed mail goes.
    void printMail(const std::string& text);

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
    std::unique_ptr<EngineLog> log_;
    std::unique_ptr<Dropbox> dropbox_;
    std::unique_ptr<ScriptHost> scripts_;
    std::unique_ptr<ConfigDir> configDir_;
    std::unique_ptr<SessionStore> sessions_;
    std::unique_ptr<AuthStore> auth_;
    std::unique_ptr<LockService> sessionLocks_;
    std::unique_ptr<DataFiles> data_;
    std::unique_ptr<AuthFlow> authFlow_;
    std::unique_ptr<DevTools> devtools_;
    ObserverList observers_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<AppInstance> instance_;
    std::unique_ptr<HttpServer> server_;
};

}  // namespace fourfold::cli

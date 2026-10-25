// DevServer: everything `fourfold dev` runs, put together. The script host,
// the engine, the app instance that recompiles on changes, the dev tools,
// and the HTTP server, on one I/O loop.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>

#include "devtools/dev_tools.h"
#include "httpserver/http_server.h"
#include "lifecycle/app_instance.h"
#include "observer/observer.h"
#include "pipeline/engine.h"
#include "platform/event_loop.h"
#include "script/script_host.h"

namespace fourfold::cli {

class DevServer : private Observer {
public:
    // Opens and compiles the app at `folder` (creating missing partitions),
    // and listens on 127.0.0.1:port (0 picks a free port). Diagnostics go to
    // `err`. An app that doesn't compile still starts: it serves an error
    // page until the code is fixed. Returns null, once `err` says why, if the
    // folder can't be opened or the port can't be had. Request lines,
    // recompiles, script errors and `print` output go to `out` and `err`
    // while it runs.
    static std::unique_ptr<DevServer> start(const std::string& folder, uint16_t port, std::ostream& out,
                                            std::ostream& err, LifecycleOptions lifecycle = {});
    ~DevServer() override;

    DevServer(const DevServer&) = delete;
    DevServer& operator=(const DevServer&) = delete;

    // Serves until loop().stop().
    EventLoop& loop() { return *loop_; }
    uint16_t port() const { return server_->port(); }
    const std::string& root() const { return root_; }
    // Whether the first compile succeeded.
    bool compiled() const { return firstCompileOk_; }

private:
    DevServer(std::ostream& out, std::ostream& err) : out_(out), err_(err) {}

    // Output comes from the I/O loop, the script thread and the lifecycle
    // thread; a line at a time.
    void say(const std::string& line);
    void sayError(const std::string& line);

    // Observer: the terminal's view of compiles and script errors.
    void compileFinished(const CompileReport& report) override;
    void scriptError(const ScriptError& e) override;

    std::ostream& out_;
    std::ostream& err_;
    std::mutex outMutex_;
    std::string root_;
    bool started_ = false;   // the first compile has been reported
    bool firstCompileOk_ = false;

    // In order of construction. Destroyed in a different order (see the
    // destructor): whatever posts to or calls into something must stop first.
    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<ScriptHost> scripts_;
    std::unique_ptr<DevTools> devtools_;
    ObserverList observers_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<AppInstance> instance_;
    std::unique_ptr<HttpServer> server_;
};

}  // namespace fourfold::cli

// DevServer: everything `fourfold dev` runs, put together. The script host,
// the compiled app, the engine, and the HTTP server, on one I/O loop.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>

#include "httpserver/http_server.h"
#include "observer/observer.h"
#include "pipeline/engine.h"
#include "platform/event_loop.h"
#include "script/script_host.h"

namespace fourfold::cli {

class DevServer : private Observer {
public:
    // Opens and compiles the app at `folder` (creating missing partitions),
    // and listens on 127.0.0.1:port (0 picks a free port). Diagnostics go to
    // `err`; on failure, returns null once `err` says why. Request lines,
    // script errors and `print` output go to `out` and `err` while it runs.
    static std::unique_ptr<DevServer> start(const std::string& folder, uint16_t port, std::ostream& out,
                                            std::ostream& err);
    ~DevServer();

    DevServer(const DevServer&) = delete;
    DevServer& operator=(const DevServer&) = delete;

    // Serves until loop().stop().
    EventLoop& loop() { return *loop_; }
    uint16_t port() const { return server_->port(); }
    const std::string& root() const { return root_; }

private:
    DevServer(std::ostream& out, std::ostream& err) : out_(out), err_(err) {}

    // Output comes from the I/O loop and the script thread; a line at a time.
    void say(const std::string& line);
    void sayError(const std::string& line);

    void scriptError(const ScriptError& e) override;

    std::ostream& out_;
    std::ostream& err_;
    std::mutex outMutex_;
    std::string root_;
    // In order of construction; destroyed in reverse. The loop outlives the
    // script host, whose thread posts responses to it.
    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<ScriptHost> scripts_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<HttpServer> server_;
};

}  // namespace fourfold::cli

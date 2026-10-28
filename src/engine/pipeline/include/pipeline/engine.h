// Engine: turns an HttpRequest into an HttpResponse (Arch §5, pipeline).
#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "compiledapp/compiled_app.h"
#include "config/app_config.h"
#include "http/message.h"
#include "partition/code_tree.h"
#include "pipeline/stages.h"
#include "policy/policy_gate.h"
#include "script/script_host.h"

namespace fourfold {

class Engine {
public:
    using Done = std::function<void(HttpResponse)>;

    // `scripts` runs the app's handlers; it may be null for an app without
    // code (app->scripts null). `app` may be null until the first successful
    // compile; requests are answered 503 meanwhile.
    Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, ScriptHost* scripts, EngineOptions options);

    // Thread-safe. Each request pins the app current when it arrives, so a
    // swap never changes the app under a request in progress; the old app is
    // released when its last request finishes.
    void setApp(std::shared_ptr<const CompiledApp> app);
    std::shared_ptr<const CompiledApp> app() const;

    // The app's config, pinned by each request like the app (M6). Until one
    // is set, the defaults for dev mode with mail off.
    void setConfig(std::shared_ptr<const AppConfig> config);
    std::shared_ptr<const AppConfig> config() const;

    // Handles one request, and calls `done` exactly once with the response:
    // before returning, for anything that runs no code, or later, on the
    // script thread or a worker, when a handler finishes. (A transport that
    // needs the response on its own thread posts it there.)
    void handle(const HttpRequest& request, Done done) const;

    struct Flow;   // private: one request past Admit

private:
    void identify(std::shared_ptr<Flow> flow, std::function<void(std::shared_ptr<Flow>)> next) const;
    void serveAuth(std::shared_ptr<Flow> flow) const;
    void serveRoute(std::shared_ptr<Flow> flow) const;

    PolicyGate gate_;
    mutable std::mutex appMutex_;
    std::shared_ptr<const CompiledApp> app_;   // guarded by appMutex_; swapped on recompile
    std::shared_ptr<const AppConfig> config_;  // guarded by appMutex_
    std::unique_ptr<UserDirectory> users_;     // over options.auth, for the `users` module
    CodeTree code_;
    ScriptHost* scripts_;
    std::shared_ptr<const EngineOptions> options_;   // shared with runs in progress
};

}  // namespace fourfold

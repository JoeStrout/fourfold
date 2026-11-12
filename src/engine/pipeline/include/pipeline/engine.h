// Engine: turns an HttpRequest into an HttpResponse (Arch §5, pipeline).
#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "compiledapp/compiled_app.h"
#include "config/app_config.h"
#include "http/message.h"
#include "partition/code_tree.h"
#include "pipeline/forms.h"
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

    // For the job runner (overview §2, Background jobs): the version and
    // config serving now, and a context with the services a job's run has
    // (data, the database, users, http, mail, enqueue, publish; no
    // session), with config.jobs' budget.
    struct JobServices {
        std::shared_ptr<const CompiledApp> app;
        std::shared_ptr<const AppConfig> config;
        RunContext context;
    };
    JobServices jobServices() const;

    // What a successful run did besides its response: its enqueued jobs,
    // committed together, then its push messages, sent. Blocks (SQLite), so
    // call it on a worker. `source` names the run ("POST /signup", "job
    // digest"). An error if the jobs couldn't be committed; nothing is
    // published then.
    Status commitEffects(const HandlerOutcome& outcome, const std::string& source) const;
    // A failed run's enqueued jobs (by name), dropped: noted for the
    // developer and in the log.
    void dropEffects(const std::vector<std::string>& names, const std::string& source) const;

    struct Flow;   // private: one request past Admit

private:
    // The outbound client for `config`, logging each call.
    std::shared_ptr<OutboundClient> makeOutbound(const AppConfig& config) const;
    // The rest of handle(), once the request is past the schema lock.
    void proceed(std::shared_ptr<Flow> flow) const;
    // The 503 for a request that waited too long for migrations.
    HttpResponse unavailable(const CompiledApp* app, const std::string& nonce) const;
    void identify(std::shared_ptr<Flow> flow, std::function<void(std::shared_ptr<Flow>)> next) const;
    void serveAuth(std::shared_ptr<Flow> flow) const;
    void serveRoute(std::shared_ptr<Flow> flow) const;
    // /live/<name>: a push channel (overview §7), under access.ms's rules.
    void serveChannel(std::shared_ptr<Flow> flow) const;
    // The rest of serveRoute, once the gate has decided.
    void afterGate(std::shared_ptr<Flow> flow, GateDecision gate, RequestFacts facts, Submission submission) const;

    PolicyGate gate_;
    mutable std::mutex appMutex_;
    std::shared_ptr<const CompiledApp> app_;   // guarded by appMutex_; swapped on recompile
    std::shared_ptr<const AppConfig> config_;  // guarded by appMutex_
    // The app's outbound client, made with each config (its settings and
    // secrets); guarded by appMutex_, and pinned by each request like the
    // config.
    std::shared_ptr<OutboundClient> outbound_;
    std::unique_ptr<UserDirectory> users_;     // over options.auth, for the `users` module
    std::unique_ptr<RateCounter> rates_;       // over options.rates, for ratelimit()
    CodeTree code_;
    ScriptHost* scripts_;
    std::shared_ptr<const EngineOptions> options_;   // shared with runs in progress
};

}  // namespace fourfold

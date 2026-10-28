#include "app_server.h"

#include <ostream>

#include "partition/app_folder.h"

namespace fourfold::cli {

namespace {

constexpr auto kSweepInterval = std::chrono::hours(1);

}  // namespace

std::unique_ptr<AppServer> AppServer::startDev(const std::string& folder, uint16_t port, std::ostream& out,
                                               std::ostream& err, LifecycleOptions lifecycle) {
    ServerOptions options;
    options.port = port;
    options.lifecycle = lifecycle;
    return start(folder, std::move(options), out, err);
}

std::unique_ptr<AppServer> AppServer::start(const std::string& folder, ServerOptions options, std::ostream& out,
                                            std::ostream& err) {
    const char* command = options.dev ? "fourfold dev: " : "fourfold serve: ";
    std::unique_ptr<AppServer> server(new AppServer(out, err));
    server->dev_ = options.dev;

    auto app = AppFolder::open(folder, AppFolder::Missing::Create);
    if (!app) {
        err << command << app.error().message << "\n";
        return nullptr;
    }
    server->root_ = app.value().root();
    // The app instance, the engine (for static files) and the dev tools (for
    // the code around errors) each read code/ through a CodeTree of their own.
    auto watchedCode = app.value().codeTree();
    auto engineCode = app.value().codeTree();
    auto devCode = app.value().codeTree();
    for (auto* code : {&watchedCode, &engineCode, &devCode}) {
        if (!*code) {
            err << command << code->error().message << "\n";
            return nullptr;
        }
    }

    // The app's state: private/state/ (the engine's), data/ and dropbox/;
    // and its config, private/config/.
    auto state = app.value().stateDir();
    auto data = app.value().dataDir();
    auto dropbox = app.value().dropbox();
    auto configDir = app.value().configDir();
    if (!state || !data || !dropbox || !configDir) {
        err << command
            << (!state ? state.error() : !data ? data.error() : !dropbox ? dropbox.error() : configDir.error()).message
            << "\n";
        return nullptr;
    }
    auto key = loadOrCreateEngineKey(state.value());
    auto dbPath = state.value().sqlitePath(RelPath::parse("state.db").value());
    if (!key || !dbPath) {
        err << command << (!key ? key.error() : dbPath.error()).message << "\n";
        return nullptr;
    }
    server->configDir_ = std::make_unique<ConfigDir>(std::move(configDir).value());
    server->data_ = std::make_unique<DataFiles>(std::move(data).value());
    server->dropbox_ = std::make_unique<Dropbox>(std::move(dropbox).value());
    server->log_ = std::make_unique<EngineLog>(std::move(state).value());

    auto loop = EventLoop::create();
    if (!loop) {
        err << command << loop.error().message << "\n";
        return nullptr;
    }
    server->loop_ = std::move(loop).value();

    AppServer* self = server.get();
    ScriptHostOptions scriptOptions;
    if (options.dev) {
        scriptOptions.printOutput = [self](const std::string& file, const std::string& text) {
            self->say(file + ": " + text);
        };
    } else {
        // Into the app's own output, on a worker: the script thread doesn't
        // wait for the disk.
        scriptOptions.printOutput = [self](const std::string& file, const std::string& text) {
            std::string line = file + ": " + text + "\n";
            self->scripts_->workers().submit([self, line] {
                (void)self->dropbox_->append(RelPath::parse("print.log").value(), line);
            });
        };
    }
    auto scripts = ScriptHost::create(scriptOptions);
    if (!scripts) {
        err << command << scripts.error().message << "\n";
        return nullptr;
    }
    server->scripts_ = std::move(scripts).value();

    // The config, evaluated by the script host. An invalid one stops
    // `fourfold serve` (its defaults may be wider than what was meant, e.g.
    // open sign-up); `fourfold dev` starts with the defaults, and reloads
    // once it's fixed.
    ConfigSources sources;
    sources.dir = server->configDir_.get();
    sources.mode = options.dev ? ConfigMode::Dev : ConfigMode::Serve;
    sources.devMailOutput = [self](const std::string& text) { self->printMail(text); };
    if (!options.serverConfig.empty()) {
        auto file = ConfigDir::readServerConfig(options.serverConfig);
        if (!file) {
            err << command << "can't read the server config: " << file.error().message << "\n";
            return nullptr;
        }
        sources.serverConfig = std::move(file).value();
    }
    std::string stamp = configStamp(*server->configDir_);
    ConfigLoad load = loadConfig(sources, *server->scripts_);
    for (const Diagnostic& d : load.diagnostics) err << d.toString() << "\n";
    if (!load.ok()) {
        if (!options.dev) {
            err << command << "the config has errors (above); fix them and start again\n";
            return nullptr;
        }
        err << command << "the config has errors (above); using the defaults until they're fixed\n";
        ConfigSources defaults = sources;
        defaults.dir = nullptr;
        load = loadConfig(defaults, *server->scripts_);
        if (!load.ok()) {
            for (const Diagnostic& d : load.diagnostics) err << d.toString() << "\n";
            return nullptr;
        }
    }
    std::shared_ptr<const AppConfig> config = load.config;
    server->scripts_->setBudget(config->budget);

    auto sessions = SessionStore::open(dbPath.value(), config->session);
    auto auth = sessions ? AuthStore::open(dbPath.value(), config->auth.store)
                         : Result<std::unique_ptr<AuthStore>>(sessions.error());
    if (!sessions || !auth) {
        err << command << "can't open private/state/state.db: " << (!sessions ? sessions.error() : auth.error()).message
            << "\n";
        return nullptr;
    }
    server->sessions_ = std::move(sessions).value();
    server->auth_ = std::move(auth).value();
    server->sessionLocks_ = makeMemoryLockService();
    server->authFlow_ = std::make_unique<AuthFlow>(*server->auth_, *server->sessions_, *server->sessionLocks_,
                                                   server->scripts_->workers(), server->log_.get());

    // The host names the app answers to: localhost's in dev; in serve mode,
    // --host's and the config's domain.
    std::vector<std::string> hosts = options.hosts;
    if (!options.dev && !config->site.domain.empty()) hosts.push_back(config->site.domain);
    if (options.dev || hosts.empty()) {
        if (!options.dev && options.address != "127.0.0.1" && options.address != "::1" &&
            options.address != "localhost") {
            err << command << "give the site's host name with --host, or config.domain in private/config/app.ms "
                              "(requests for any other name are refused)\n";
            return nullptr;
        }
        hosts = localhostNames();
    }

    server->observers_.add(self);
    if (options.dev) {
        server->devtools_ = std::make_unique<DevTools>(*server->loop_, std::move(devCode).value());
        server->observers_.add(server->devtools_.get());
    }

    EngineOptions engineOptions;
    engineOptions.allowedHosts = hosts;
    engineOptions.showErrorDetails = options.dev;
    engineOptions.observer = &server->observers_;
    engineOptions.hooks = server->devtools_.get();
    engineOptions.sessions = server->sessions_.get();
    engineOptions.sessionLocks = server->sessionLocks_.get();
    engineOptions.data = server->data_.get();
    engineOptions.dropbox = server->dropbox_.get();
    engineOptions.log = server->log_.get();
    engineOptions.auth = server->auth_.get();
    engineOptions.authRoutes = server->authFlow_.get();
    engineOptions.https = options.https;
    engineOptions.engineKey = std::move(key).value();
    server->engine_ = std::make_unique<Engine>(nullptr, std::move(engineCode).value(), server->scripts_.get(),
                                               std::move(engineOptions));
    server->engine_->setConfig(config);
    if (server->devtools_) server->devtools_->setAppSource([self] { return self->engine_->app(); });

    ConfigWatch watch;
    watch.sources = sources;
    watch.stamp = stamp;
    watch.loaded = [self](const ConfigLoad& reloaded) { self->configLoaded(reloaded, false); };
    options.lifecycle.config = std::move(watch);

    if (!options.dev) options.lifecycle.trigger = LifecycleOptions::Trigger::DeployOnly;
    auto instance = AppInstance::create(std::move(watchedCode).value(), server->scripts_.get(), *server->engine_,
                                        &server->observers_, options.lifecycle);
    if (!instance) {
        err << command << instance.error().message << "\n";
        return nullptr;
    }
    server->instance_ = std::move(instance).value();
    server->instance_->start();   // reports through compileFinished

    server->server_ = std::make_unique<HttpServer>(*server->loop_, [self](HttpRequest request, Responder respond) {
        std::string line = request.method + " " + request.target;
        bool quiet = request.target.compare(0, 7, "/__dev/") == 0;   // the dev tools' own traffic
        std::string peer = request.peerAddress;
        // The response may come from the script thread or a worker; the
        // server wants it on its own loop.
        self->engine_->handle(request, [self, respond, line, quiet, peer](HttpResponse response) {
            auto shared = std::make_shared<HttpResponse>(std::move(response));
            self->loop_->post([self, respond, line, quiet, peer, shared] {
                std::string status = std::to_string(shared->status);
                if (self->dev_) {
                    if (!quiet) self->say(line + " " + status);
                } else {
                    self->log_->write("request", peer + " " + line + " " + status);
                }
                respond(std::move(*shared));
            });
        });
    });
    if (Status s = server->server_->listen(options.address, options.port); !s) {
        err << command << "can't listen on " << options.address << " port " << options.port << ": "
            << s.error().message << "\n";
        return nullptr;
    }
    server->log_->write("start", std::string(options.dev ? "dev" : "serve") + " mode, listening on " +
                                     options.address + " port " + std::to_string(server->port()) +
                                     (options.https ? ", behind HTTPS" : ", plain HTTP"));
    server->scheduleSweep(std::chrono::milliseconds(0));
    return server;
}

AppServer::~AppServer() {
    // The server first (closing connections), then the app instance (no more
    // compiles), then the script host (its thread and workers stop, so no
    // run calls back into the engine's options, hooks or stores), and the
    // rest after.
    server_.reset();
    instance_.reset();
    scripts_.reset();
    engine_.reset();
    devtools_.reset();
    authFlow_.reset();
    data_.reset();
    sessionLocks_.reset();
    auth_.reset();
    sessions_.reset();
    configDir_.reset();
    loop_.reset();
    dropbox_.reset();
    log_.reset();
}

void AppServer::scheduleSweep(std::chrono::milliseconds delay) {
    loop_->addTimer(delay, [this] {
        scripts_->workers().submit([this] {
            auto removed = sessions_->sweepExpired();
            if (!removed) log_->write("session", "sweeping expired sessions failed: " + removed.error().message);
            auto tokens = auth_->sweep();
            if (!tokens) log_->write("auth", "sweeping old sign-in links failed: " + tokens.error().message);
        });
        scheduleSweep(kSweepInterval);
    });
}

void AppServer::configLoaded(const ConfigLoad& load, bool first) {
    for (const Diagnostic& d : load.diagnostics) {
        sayError(d.toString());
        log_->write("config", d.toString());
    }
    if (!load.ok()) {
        sayError("The config has errors; still using the previous one.");
        return;
    }
    sessions_->setConfig(load.config->session);
    auth_->setConfig(load.config->auth.store);
    scripts_->setBudget(load.config->budget);
    if (!first) {
        say("Config reloaded.");
        log_->write("config", "reloaded");
    }
}

void AppServer::printMail(const std::string& text) {
    if (dev_) say("\n" + text + "\n");
    log_->write("mail", text);
}

void AppServer::compileFinished(const CompileReport& report) {
    for (const Diagnostic& d : report.diagnostics) {
        sayError(d.toString());
        if (!dev_) log_->write("compile", d.toString());
    }
    bool first = !started_;
    started_ = true;
    if (first) {
        firstCompileOk_ = report.ok;
        if (!report.ok) {
            sayError(dev_ ? "fourfold dev: the app didn't compile; serving an error page until it does."
                          : "fourfold serve: the app didn't compile; serving an error page until it does.");
        }
        return;
    }
    if (!report.ok) {
        sayError(report.generation ? "Didn't compile; still serving the last version that did."
                                   : "Didn't compile; nothing to serve yet.");
        return;
    }
    std::string changed;
    for (size_t i = 0; i < report.changed.size() && i < 3; i++) {
        changed += (i ? ", " : "") + report.changed[i].str();
    }
    if (report.changed.size() > 3) changed += ", and " + std::to_string(report.changed.size() - 3) + " more";
    std::string line = "Recompiled (" + (changed.empty() ? std::string("no changes") : changed) + ") in " +
                       std::to_string(report.duration.count()) + " ms" +
                       (report.cssOnly ? "; swapping stylesheets" : "");
    say(line);
    if (!dev_) log_->write("compile", line);
}

void AppServer::scriptError(const ScriptError& e) {
    std::string where = e.file + (e.line > 0 ? ":" + std::to_string(e.line) : "");
    sayError((where.empty() ? "" : where + ": ") + "error: " + e.message);
}

void AppServer::renderWarnings(uint64_t generation, const std::vector<RenderWarning>& warnings) {
    std::lock_guard<std::mutex> lock(outMutex_);
    if (generation != warnedGeneration_) {
        warnedGeneration_ = generation;
        warned_.clear();
    }
    for (const RenderWarning& w : warnings) {
        std::string line = "code/" + w.file + (w.line > 0 ? ":" + std::to_string(w.line) : "") +
                           ": warning: " + w.message;
        if (warned_.insert(line).second) err_ << line << std::endl;
    }
}

void AppServer::say(const std::string& line) {
    std::lock_guard<std::mutex> lock(outMutex_);
    out_ << line << std::endl;
}

void AppServer::sayError(const std::string& line) {
    std::lock_guard<std::mutex> lock(outMutex_);
    err_ << line << std::endl;
}

}  // namespace fourfold::cli

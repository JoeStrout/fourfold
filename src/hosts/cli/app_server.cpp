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

    // The app's state: private/state/ (the engine's), data/ and dropbox/.
    auto state = app.value().stateDir();
    auto data = app.value().dataDir();
    auto dropbox = app.value().dropbox();
    if (!state || !data || !dropbox) {
        err << command << (!state ? state.error() : !data ? data.error() : dropbox.error()).message << "\n";
        return nullptr;
    }
    auto key = loadOrCreateEngineKey(state.value());
    auto dbPath = state.value().sqlitePath(RelPath::parse("state.db").value());
    if (!key || !dbPath) {
        err << command << (!key ? key.error() : dbPath.error()).message << "\n";
        return nullptr;
    }
    auto sessions = SessionStore::open(dbPath.value(), SessionConfig{});
    if (!sessions) {
        err << command << "can't open private/state/state.db: " << sessions.error().message << "\n";
        return nullptr;
    }
    server->sessions_ = std::move(sessions).value();
    server->sessionLocks_ = makeMemoryLockService();
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

    server->observers_.add(self);
    if (options.dev) {
        server->devtools_ = std::make_unique<DevTools>(*server->loop_, std::move(devCode).value());
        server->observers_.add(server->devtools_.get());
    }

    EngineOptions engineOptions;
    engineOptions.allowedHosts = options.hosts.empty() ? localhostNames() : options.hosts;
    engineOptions.showErrorDetails = options.dev;
    engineOptions.observer = &server->observers_;
    engineOptions.hooks = server->devtools_.get();
    engineOptions.sessions = server->sessions_.get();
    engineOptions.sessionLocks = server->sessionLocks_.get();
    engineOptions.data = server->data_.get();
    engineOptions.dropbox = server->dropbox_.get();
    engineOptions.log = server->log_.get();
    engineOptions.https = options.https;
    engineOptions.engineKey = std::move(key).value();
    server->engine_ = std::make_unique<Engine>(nullptr, std::move(engineCode).value(), server->scripts_.get(),
                                               std::move(engineOptions));

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
    data_.reset();
    sessionLocks_.reset();
    sessions_.reset();
    loop_.reset();
    dropbox_.reset();
    log_.reset();
}

void AppServer::scheduleSweep(std::chrono::milliseconds delay) {
    loop_->addTimer(delay, [this] {
        scripts_->workers().submit([this] {
            auto removed = sessions_->sweepExpired();
            if (!removed) log_->write("session", "sweeping expired sessions failed: " + removed.error().message);
        });
        scheduleSweep(kSweepInterval);
    });
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

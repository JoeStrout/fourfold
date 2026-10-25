#include "dev_server.h"

#include <ostream>

#include "partition/app_folder.h"

namespace fourfold::cli {

std::unique_ptr<DevServer> DevServer::start(const std::string& folder, uint16_t port, std::ostream& out,
                                            std::ostream& err, LifecycleOptions lifecycle) {
    std::unique_ptr<DevServer> dev(new DevServer(out, err));

    auto app = AppFolder::open(folder, AppFolder::Missing::Create);
    if (!app) {
        err << "fourfold dev: " << app.error().message << "\n";
        return nullptr;
    }
    dev->root_ = app.value().root();
    // The app instance, the engine (for static files) and the dev tools (for
    // the code around errors) each read code/ through a CodeTree of their own.
    auto watchedCode = app.value().codeTree();
    auto engineCode = app.value().codeTree();
    auto devCode = app.value().codeTree();
    for (auto* code : {&watchedCode, &engineCode, &devCode}) {
        if (!*code) {
            err << "fourfold dev: " << code->error().message << "\n";
            return nullptr;
        }
    }

    auto loop = EventLoop::create();
    if (!loop) {
        err << "fourfold dev: " << loop.error().message << "\n";
        return nullptr;
    }
    dev->loop_ = std::move(loop).value();

    DevServer* self = dev.get();
    ScriptHostOptions scriptOptions;
    scriptOptions.printOutput = [self](const std::string& file, const std::string& text) {
        self->say(file + ": " + text);
    };
    auto scripts = ScriptHost::create(scriptOptions);
    if (!scripts) {
        err << "fourfold dev: " << scripts.error().message << "\n";
        return nullptr;
    }
    dev->scripts_ = std::move(scripts).value();

    dev->devtools_ = std::make_unique<DevTools>(*dev->loop_, std::move(devCode).value());
    dev->observers_.add(self);
    dev->observers_.add(dev->devtools_.get());

    EngineOptions engineOptions{localhostNames()};
    engineOptions.showErrorDetails = true;
    engineOptions.observer = &dev->observers_;
    engineOptions.hooks = dev->devtools_.get();
    dev->engine_ = std::make_unique<Engine>(nullptr, std::move(engineCode).value(), dev->scripts_.get(),
                                            std::move(engineOptions));

    auto instance = AppInstance::create(std::move(watchedCode).value(), dev->scripts_.get(), *dev->engine_,
                                        &dev->observers_, lifecycle);
    if (!instance) {
        err << "fourfold dev: " << instance.error().message << "\n";
        return nullptr;
    }
    dev->instance_ = std::move(instance).value();
    dev->instance_->start();   // reports through compileFinished

    dev->server_ = std::make_unique<HttpServer>(*dev->loop_, [self](HttpRequest request, Responder respond) {
        std::string line = request.method + " " + request.target;
        bool quiet = request.target.compare(0, 7, "/__dev/") == 0;   // the dev tools' own traffic
        // The response may come from the script thread; the server wants it
        // on its own loop.
        self->engine_->handle(request, [self, respond, line, quiet](HttpResponse response) {
            auto shared = std::make_shared<HttpResponse>(std::move(response));
            self->loop_->post([self, respond, line, quiet, shared] {
                if (!quiet) self->say(line + " " + std::to_string(shared->status));
                respond(std::move(*shared));
            });
        });
    });
    // Localhost only: dev mode lacks protections that later milestones add.
    if (Status s = dev->server_->listen("127.0.0.1", port); !s) {
        err << "fourfold dev: can't listen on port " << port << ": " << s.error().message << "\n";
        return nullptr;
    }
    return dev;
}

DevServer::~DevServer() {
    // The server first (closing connections), then the app instance (no more
    // compiles), then the script host (its thread stops, so no run calls
    // back into the engine's options or hooks), and the rest after.
    server_.reset();
    instance_.reset();
    scripts_.reset();
    engine_.reset();
    devtools_.reset();
    loop_.reset();
}

void DevServer::compileFinished(const CompileReport& report) {
    for (const Diagnostic& d : report.diagnostics) sayError(d.toString());
    bool first = !started_;
    started_ = true;
    if (first) {
        firstCompileOk_ = report.ok;
        if (!report.ok) sayError("fourfold dev: the app didn't compile; serving an error page until it does.");
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
    say("Recompiled (" + (changed.empty() ? std::string("no changes") : changed) + ") in " +
        std::to_string(report.duration.count()) + " ms" + (report.cssOnly ? "; swapping stylesheets" : ""));
}

void DevServer::scriptError(const ScriptError& e) {
    std::string where = e.file + (e.line > 0 ? ":" + std::to_string(e.line) : "");
    sayError(where + ": error: " + e.message);
}

void DevServer::say(const std::string& line) {
    std::lock_guard<std::mutex> lock(outMutex_);
    out_ << line << std::endl;
}

void DevServer::sayError(const std::string& line) {
    std::lock_guard<std::mutex> lock(outMutex_);
    err_ << line << std::endl;
}

}  // namespace fourfold::cli

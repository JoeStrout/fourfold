#include "dev_server.h"

#include <ostream>

#include "compiler/compiler.h"
#include "partition/app_folder.h"

namespace fourfold::cli {

std::unique_ptr<DevServer> DevServer::start(const std::string& folder, uint16_t port, std::ostream& out,
                                            std::ostream& err) {
    std::unique_ptr<DevServer> dev(new DevServer(out, err));

    auto app = AppFolder::open(folder, AppFolder::Missing::Create);
    if (!app) {
        err << "fourfold dev: " << app.error().message << "\n";
        return nullptr;
    }
    dev->root_ = app.value().root();
    auto code = app.value().codeTree();
    if (!code) {
        err << "fourfold dev: " << code.error().message << "\n";
        return nullptr;
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

    CompileOptions compileOptions;
    compileOptions.scripts = dev->scripts_.get();
    CompileOutput compiled = compile(code.value(), compileOptions);
    for (const Diagnostic& d : compiled.diagnostics) err << d.toString() << "\n";
    if (!compiled.ok()) {
        err << "fourfold dev: the app didn't compile.\n";
        return nullptr;
    }

    // Static files are read per request, through a CodeTree of the engine's own.
    auto engineCode = app.value().codeTree();
    if (!engineCode) {
        err << "fourfold dev: " << engineCode.error().message << "\n";
        return nullptr;
    }
    EngineOptions engineOptions{localhostNames()};
    engineOptions.showErrorDetails = true;
    engineOptions.observer = self;
    dev->engine_ = std::make_unique<Engine>(compiled.app, std::move(engineCode).value(), dev->scripts_.get(),
                                            std::move(engineOptions));

    dev->server_ = std::make_unique<HttpServer>(*dev->loop_, [self](HttpRequest request, Responder respond) {
        std::string line = request.method + " " + request.target;
        // The response may come from the script thread; the server wants it
        // on its own loop.
        self->engine_->handle(request, [self, respond, line](HttpResponse response) {
            auto shared = std::make_shared<HttpResponse>(std::move(response));
            self->loop_->post([self, respond, line, shared] {
                self->say(line + " " + std::to_string(shared->status));
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
    // The server first, then the script host (which stops its thread, so
    // nothing more is posted to the loop), and the loop last.
    server_.reset();
    engine_.reset();
    scripts_.reset();
    loop_.reset();
}

void DevServer::say(const std::string& line) {
    std::lock_guard<std::mutex> lock(outMutex_);
    out_ << line << std::endl;
}

void DevServer::scriptError(const ScriptError& e) {
    std::string where = e.file + (e.line > 0 ? ":" + std::to_string(e.line) : "");
    sayError(where + ": error: " + e.message);
}

void DevServer::sayError(const std::string& line) {
    std::lock_guard<std::mutex> lock(outMutex_);
    err_ << line << std::endl;
}

}  // namespace fourfold::cli

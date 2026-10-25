#include "pipeline/engine.h"

namespace fourfold {

namespace {

const char* const kNotCompiledPage =
    "<!DOCTYPE html>\n<html><head><title>Not compiled yet</title></head>\n"
    "<body><h1>The app hasn't compiled yet</h1>\n"
    "<p>Fix the errors listed where Fourfold is running, and this page will load.</p></body></html>\n";

bool isHtml(const HttpResponse& response) {
    const std::string* type = response.headers.get("Content-Type");
    return type && type->compare(0, 9, "text/html") == 0;
}

// "/__dev/reload" and "/__dev" are the dev tools'.
bool isDevPath(const std::string& path) {
    return path == "/__dev" || path.compare(0, 7, "/__dev/") == 0;
}

// The standard headers, and the dev tools' hook.
void finish(HttpResponse& response, uint64_t generation, const EngineOptions& options) {
    addStandardHeaders(response);
    if (options.hooks && isHtml(response)) options.hooks->decorate(response, generation);
}

}  // namespace

Engine::Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, ScriptHost* scripts, EngineOptions options)
    : app_(std::move(app)),
      code_(std::move(code)),
      scripts_(scripts),
      options_(std::make_shared<const EngineOptions>(std::move(options))) {}

void Engine::setApp(std::shared_ptr<const CompiledApp> app) {
    std::shared_ptr<const CompiledApp> old;
    {
        std::lock_guard<std::mutex> lock(appMutex_);
        old = std::move(app_);
        app_ = std::move(app);
    }
    // `old` is let go here, outside the lock; its bytecode goes when the
    // requests still using it finish.
}

std::shared_ptr<const CompiledApp> Engine::app() const {
    std::lock_guard<std::mutex> lock(appMutex_);
    return app_;
}


void Engine::handle(const HttpRequest& request, Done done) const {
    AdmittedRequest admitted;
    if (auto rejected = admit(request, *options_, admitted)) {
        addStandardHeaders(*rejected);
        done(std::move(*rejected));
        return;
    }

    // Reserved paths go to their owners before routing (Arch §5, pipeline).
    if (isDevPath(admitted.path.path)) {
        std::optional<HttpResponse> response;
        if (options_->hooks) response = options_->hooks->reserved(request, admitted);
        if (!response) response = HttpResponse::text(404, "Not Found\n");
        addStandardHeaders(*response);
        done(std::move(*response));
        return;
    }

    // Pin this generation of the app for the whole request.
    std::shared_ptr<const CompiledApp> app = this->app();
    if (!app) {
        HttpResponse response;
        response.status = 503;
        response.headers.set("Content-Type", "text/html; charset=utf-8");
        response.headers.set("Retry-After", "1");
        response.body = kNotCompiledPage;
        finish(response, 0, *options_);
        done(std::move(response));
        return;
    }
    RouteDecision decision = route(admitted, *app);

    if (!decision.handler) {
        HttpResponse response = dispatch(decision, *app, code_);
        finish(response, app->generation, *options_);
        done(std::move(response));
        return;
    }

    if (!scripts_) {
        HttpResponse response = errorResponse({"no script host to run " + decision.handler->label(), "", 0},
                                              *app, *options_);
        finish(response, app->generation, *options_);
        done(std::move(response));
        return;
    }

    // The route entry lives in `app`, which the run keeps alive.
    const RouteEntry* entry = decision.entry;
    std::shared_ptr<const EngineOptions> options = options_;
    scripts_->run(app, *decision.handler, requestFacts(request, admitted),
                  [app, entry, options, done = std::move(done)](HandlerOutcome outcome) {
                      HttpResponse response = respond(outcome, *entry, *app, *options);
                      finish(response, app->generation, *options);
                      done(std::move(response));
                  });
}

}  // namespace fourfold

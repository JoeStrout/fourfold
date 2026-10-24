#include "pipeline/engine.h"

namespace fourfold {

Engine::Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, ScriptHost* scripts, EngineOptions options)
    : app_(std::move(app)),
      code_(std::move(code)),
      scripts_(scripts),
      options_(std::make_shared<const EngineOptions>(std::move(options))) {}

void Engine::handle(const HttpRequest& request, Done done) const {
    AdmittedRequest admitted;
    if (auto rejected = admit(request, *options_, admitted)) {
        addStandardHeaders(*rejected);
        done(std::move(*rejected));
        return;
    }

    // Pin this generation of the app for the whole request (M3 swaps app_).
    std::shared_ptr<const CompiledApp> app = app_;
    RouteDecision decision = route(admitted, *app);

    if (!decision.handler) {
        HttpResponse response = dispatch(decision, *app, code_);
        addStandardHeaders(response);
        done(std::move(response));
        return;
    }

    if (!scripts_) {
        HttpResponse response = errorResponse({"no script host to run " + decision.handler->label(), "", 0},
                                              *app, *options_);
        addStandardHeaders(response);
        done(std::move(response));
        return;
    }

    // The route entry lives in `app`, which the run keeps alive.
    const RouteEntry* entry = decision.entry;
    std::shared_ptr<const EngineOptions> options = options_;
    scripts_->run(app, *decision.handler, requestFacts(request, admitted),
                  [app, entry, options, done = std::move(done)](HandlerOutcome outcome) {
                      HttpResponse response = respond(outcome, *entry, *app, *options);
                      addStandardHeaders(response);
                      done(std::move(response));
                  });
}

}  // namespace fourfold

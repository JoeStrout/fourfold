#include "pipeline/engine.h"

namespace fourfold {

Engine::Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, EngineOptions options)
    : app_(std::move(app)), code_(std::move(code)), options_(std::move(options)) {}

HttpResponse Engine::handle(const HttpRequest& request) const {
    AdmittedRequest admitted;
    HttpResponse response;
    if (auto rejected = admit(request, options_, admitted)) {
        response = std::move(*rejected);
    } else {
        RouteDecision decision = route(admitted, *app_);
        response = dispatch(decision, *app_, code_);
    }
    addStandardHeaders(response);
    return response;
}

}  // namespace fourfold

#include "pipeline/engine.h"

namespace fourfold {

Engine::Engine(std::shared_ptr<const AppImage> image, CodeTree code, EngineOptions options)
    : image_(std::move(image)), code_(std::move(code)), options_(std::move(options)) {}

HttpResponse Engine::handle(const HttpRequest& request) const {
    AdmittedRequest admitted;
    HttpResponse response;
    if (auto rejected = admit(request, options_, admitted)) {
        response = std::move(*rejected);
    } else {
        RouteDecision decision = route(admitted, *image_);
        response = dispatch(decision, *image_, code_);
    }
    addStandardHeaders(response);
    return response;
}

}  // namespace fourfold

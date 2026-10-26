// EngineHooks: how the dev tools take part in requests (Arch §5 devtools),
// without the core knowing about them. The host passes an implementation in
// EngineOptions; production passes none.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "compiledapp/compiled_app.h"
#include "http/message.h"
#include "script/script_host.h"

namespace fourfold {

struct AdmittedRequest;

class EngineHooks {
public:
    virtual ~EngineHooks() = default;

    // A request under /__dev/, after Admit (so the Host check has passed)
    // and before routing. Nullopt means not found. Called on the transport's
    // thread.
    virtual std::optional<HttpResponse> reserved(const HttpRequest& request, const AdmittedRequest& admitted) = 0;

    // Every full HTML page, as it leaves the engine (not fragments): its
    // `generation` is the app generation that produced it (0 if none), and
    // `nonce` the response's CSP nonce, for any script added. May be called
    // on any thread.
    virtual void decorate(HttpResponse& response, uint64_t generation, const std::string& nonce) = 0;

    // The 500 page for a script error, with the details, when
    // EngineOptions::showErrorDetails is set. May be called on any thread.
    virtual HttpResponse scriptErrorPage(const ScriptError& error) = 0;
};

}  // namespace fourfold

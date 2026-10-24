// Engine: turns an HttpRequest into an HttpResponse (Arch §5, pipeline).
#pragma once

#include <functional>
#include <memory>

#include "compiledapp/compiled_app.h"
#include "http/message.h"
#include "partition/code_tree.h"
#include "pipeline/stages.h"
#include "script/script_host.h"

namespace fourfold {

class Engine {
public:
    using Done = std::function<void(HttpResponse)>;

    // `scripts` runs the app's handlers; it may be null for an app without
    // code (app->scripts null).
    Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, ScriptHost* scripts, EngineOptions options);

    // Handles one request, and calls `done` exactly once with the response:
    // before returning, for anything that runs no code, or later, on the
    // script thread, when a handler finishes. (A transport that needs the
    // response on its own thread posts it there.)
    void handle(const HttpRequest& request, Done done) const;

private:
    std::shared_ptr<const CompiledApp> app_;   // swapped on recompile from M3
    CodeTree code_;
    ScriptHost* scripts_;
    std::shared_ptr<const EngineOptions> options_;   // shared with runs in progress
};

}  // namespace fourfold

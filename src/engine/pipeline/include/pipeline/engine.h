// Engine: turns an HttpRequest into an HttpResponse (Arch §5, pipeline).
#pragma once

#include <memory>

#include "http/message.h"
#include "compiledapp/compiled_app.h"
#include "partition/code_tree.h"
#include "pipeline/stages.h"

namespace fourfold {

class Engine {
public:
    Engine(std::shared_ptr<const CompiledApp> app, CodeTree code, EngineOptions options);

    HttpResponse handle(const HttpRequest& request) const;

private:
    std::shared_ptr<const CompiledApp> app_;   // swapped on recompile from M3
    CodeTree code_;
    EngineOptions options_;
};

}  // namespace fourfold

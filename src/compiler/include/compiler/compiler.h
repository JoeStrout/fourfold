// The compiler: a snapshot of code/ in, an CompiledApp and diagnostics out
// (Arch §5). It reads only through CodeTree, so it can only ever compile
// what's in the code partition.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "partition/code_tree.h"
#include "script/script_host.h"

namespace fourfold {

struct CompileOptions {
    uint64_t generation = 1;
    // Compiles the .ms files (normally the ScriptHost). Required if there are
    // any; an app without code compiles without it.
    ScriptCompiler* scripts = nullptr;
    // Evaluates code/access.ms (normally the ScriptHost too). Required if
    // the app has one.
    DataEvaluator* evaluator = nullptr;
};

struct CompileOutput {
    // Null if there were errors. Its own diagnostics hold any warnings.
    std::shared_ptr<const CompiledApp> app;
    // Everything found, errors and warnings, in the order found.
    std::vector<Diagnostic> diagnostics;

    bool ok() const { return app != nullptr; }
};

CompileOutput compile(const CodeTree& code, const CompileOptions& options = {});

}  // namespace fourfold

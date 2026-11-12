// The compiler: a snapshot of code/ in, an CompiledApp and diagnostics out
// (Arch §5). It reads only through CodeTree, so it can only ever compile
// what's in the code partition.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
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
    // Evaluates code/access.ms and code/limits.ms (normally the ScriptHost
    // too). Required if the app has either.
    DataEvaluator* evaluator = nullptr;
    // The app's database (from its config), whose variant of each query and
    // migration is used.
    SqlDialect dialect = SqlDialect::Sqlite;

    // CGI mode (overview §8; M10): the handlers every .ms file defines, from
    // a full compile of this same code. With them, only the files in
    // `compileOnly` ("pages/hello.ms") and the libs they import are compiled
    // (and none at all if it's empty); the other files' handlers come from
    // here. The app's scripts then hold only those files, so it serves
    // requests for their routes, and no others that run code.
    const std::map<RelPath, PageHandlers>* knownHandlers = nullptr;
    std::set<RelPath> compileOnly{};
};

struct CompileOutput {
    // Null if there were errors. Its own diagnostics hold any warnings.
    std::shared_ptr<const CompiledApp> app;
    // Everything found, errors and warnings, in the order found.
    std::vector<Diagnostic> diagnostics;
    // The handlers each .ms file in code/pages/ and code/jobs/ defines, for
    // CompileOptions::knownHandlers (CGI mode keeps them between requests).
    std::map<RelPath, PageHandlers> handlers{};

    bool ok() const { return app != nullptr; }
};

CompileOutput compile(const CodeTree& code, const CompileOptions& options = {});

}  // namespace fourfold

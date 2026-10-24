// Compiling .ms files into a ScriptBundle, and finding each page's handlers.

#include <algorithm>
#include <chrono>
#include <cctype>

#include "bridge.h"
#include "host_impl.h"

namespace fourfold {

using MiniScript::FuncDef;
using MiniScript::List;
using MiniScript::String;
using MiniScript::Value;

namespace {

std::string codeFileName(const RelPath& path) {
    return "code/" + path.str();
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

// The parse → simplify → generate pipeline of InterpreterStorage::Compile,
// which keeps its result to itself; we need the functions, to load them into
// many VMs. On an error, returns an empty list and sets `error`.
List<FuncDef> compileProgram(const std::string& source, const std::string& fileName, Value* error) {
    *error = Value::Null;
    MiniScript::Parser parser = MiniScript::Parser::New();
    parser.Init(String(source.data(), source.size()), String(fileName.c_str()));
    List<MiniScript::ASTNode> statements = parser.ParseProgram();
    parser.RequireComplete();
    if (parser.HadError()) {
        *error = parser.Error();
        return List<FuncDef>();
    }
    for (MiniScript::Int32 i = 0; i < statements.Count(); i++) statements[i] = statements[i].Simplify();
    MiniScript::BytecodeEmitter emitter = MiniScript::BytecodeEmitter::New();
    MiniScript::CodeGenerator generator = MiniScript::CodeGenerator::New(emitter);
    generator.set_FileName(String(fileName.c_str()));
    generator.CompileProgram(statements, "@main");
    if (!generator.Error().IsNull()) {
        *error = generator.Error();
        return List<FuncDef>();
    }
    return generator.GetFunctions();
}

Diagnostic diagnosticFrom(const ScriptError& e) {
    return {Diagnostic::Severity::Error, e.file, e.line, e.message};
}

// Runs a page's top level to the end, synchronously, within the budget.
// Returns false, with `error` set, if it fails.
bool runTopLevel(Run& run, const ScriptHostOptions& options, ScriptError& error) {
    MiniScript::VM vm = run.interp.vm();
    auto start = std::chrono::steady_clock::now();
    while (vm.IsRunning()) {
        vm.set_yielding(false);
        vm.Run(options.sliceInstructions);
        run.instructions += options.sliceInstructions;
        if (!vm.Error().IsNull()) {
            error = scriptErrorFrom(vm.Error(), run.fileName);
            return false;
        }
        if (!vm.IsRunning()) break;
        bool overTime = std::chrono::steady_clock::now() - start > options.budget.wallClock;
        if (run.instructions >= options.budget.instructions || overTime) {
            error = {"the top level didn't finish within its budget; it should only define functions", run.fileName,
                     0};
            return false;
        }
    }
    return true;
}

// Compiles "_ff_result = <call>" once per distinct call, shared by all pages.
const List<FuncDef>* stubFor(const std::string& call, ScriptBundle& bundle,
                             std::map<std::string, List<FuncDef>>& cache) {
    auto it = cache.find(call);
    if (it != cache.end()) return &it->second;
    Value error;
    List<FuncDef> functions = compileProgram("_ff_result = " + call, "(fourfold)", &error);
    if (!error.IsNull()) return nullptr;   // can't happen: names are checked identifiers
    bundle.root(functions[0]);
    return &cache.emplace(call, functions).first->second;
}

// The call a stub makes: "render(_ff_req)", or "get.more" for a handler
// declared without a parameter.
std::string callFor(const std::string& target, Value function) {
    bool takesReq = function.FunctionDef().ParamNames().Count() > 0;
    return takesReq ? target + "(_ff_req)" : target;
}

}  // namespace

CompiledScripts ScriptHost::Impl::compile(const std::vector<CodeFile>& pages, const std::vector<CodeFile>& libs) {
    CompiledScripts out;
    auto bundle = std::make_unique<ScriptBundle>();
    std::map<std::string, List<FuncDef>> stubCache;
    bool failed = false;
    auto error = [&](ScriptError e) {
        out.diagnostics.push_back(diagnosticFrom(e));
        failed = true;
    };

    for (const CodeFile& lib : libs) {
        std::string fileName = codeFileName(lib.path());
        std::string_view name = lib.path().name();
        std::string importName(name.substr(0, name.size() - lib.path().extension().size()));
        if (!isIdentifier(importName)) {
            error({"can't be imported: the name before .ms must be a valid identifier", fileName, 0});
            continue;
        }
        Value compileError;
        const std::string& source = lib.contents();
        FuncDef module = MiniScript::Interpreter::CompileToFunc(String(source.data(), source.size()),
                                                                String(fileName.c_str()), &compileError);
        if (!compileError.IsNull()) {
            error(scriptErrorFrom(compileError, fileName));
            continue;
        }
        bundle->root(module);
        bundle->libs[importName] = module;
    }

    for (const CodeFile& file : pages) {
        std::string fileName = codeFileName(file.path());
        Value compileError;
        List<FuncDef> functions = compileProgram(file.contents(), fileName, &compileError);
        if (!compileError.IsNull()) {
            error(scriptErrorFrom(compileError, fileName));
            continue;
        }
        bundle->root(functions[0]);

        // Find the handlers by running the top level, as each request will.
        Run run;
        run.kind = Run::Kind::Discovery;
        run.host = this;
        run.fileName = fileName;
        run.libs = &bundle->libs;
        prepareInterpreter(run, functions);
        ScriptError runError;
        if (!runTopLevel(run, options, runError)) {
            run.interp.Reset();
            error(runError);
            continue;
        }

        PageProgram program;
        program.fileName = fileName;
        program.functions = functions;
        PageHandlers handlers;
        bool pageOk = true;
        auto pageError = [&](const std::string& message) {
            error({message, fileName, 0});
            pageOk = false;
        };

        Value render = run.interp.GetGlobalValue("render");
        if (!render.IsNull()) {
            if (!render.IsFuncRef()) {
                pageError("render must be a function");
            } else if (const List<FuncDef>* stub = stubFor(callFor("render", render), *bundle, stubCache)) {
                handlers.render = true;
                program.stubs["render"] = {*stub};
            }
        }
        for (const char* method : {"get", "post"}) {
            Value map = run.interp.GetGlobalValue(method);
            if (!map.IsMap()) {
                pageError(std::string(method) + " must stay a map of handler functions");
                continue;
            }
            std::vector<std::string>& names = std::string(method) == "get" ? handlers.get : handlers.post;
            MiniScript::MapIterator it = map.Iterator();
            Value key, value;
            while (MiniScript::map_iterator_next(&it, &key, &value)) {
                std::string name = key.IsString() ? toStdString(key) : "";
                if (!isIdentifier(name)) {
                    pageError(std::string(method) + " has a key that isn't a valid name: " + toStdString(key));
                    continue;
                }
                std::string label = std::string(method) + "." + name;
                if (!value.IsFuncRef()) {
                    pageError(label + " must be a function");
                    continue;
                }
                if (const List<FuncDef>* stub = stubFor(callFor(label, value), *bundle, stubCache)) {
                    names.push_back(name);
                    program.stubs[label] = {*stub};
                }
            }
            std::sort(names.begin(), names.end());
        }
        run.interp.Reset();   // releases the run's globals

        if (pageOk) {
            out.pages[file.path()] = handlers;
            bundle->pages.emplace(file.path(), std::move(program));
        }
    }

    if (!failed) out.bundle = makeBundlePtr(std::move(bundle));
    return out;
}

}  // namespace fourfold

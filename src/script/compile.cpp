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

namespace {

// One compile in progress, on the script thread. Each step is its own posted
// task: a lib, or a page (compiled, then its top level scheduled as a
// discovery run, whose end posts the next step).
struct CompileJob : std::enable_shared_from_this<CompileJob> {
    ScriptHost::Impl* host = nullptr;
    const std::vector<CodeFile>* pages = nullptr;
    const std::vector<CodeFile>* libs = nullptr;
    std::function<void(CompiledScripts)> done;

    CompiledScripts out;
    std::unique_ptr<ScriptBundle> bundle = std::make_unique<ScriptBundle>();
    std::map<std::string, List<FuncDef>> stubCache;
    bool failed = false;
    size_t next = 0;   // the next step: libs first, then pages

    void error(ScriptError e) {
        out.diagnostics.push_back(diagnosticFrom(e));
        failed = true;
    }

    void postNext() {
        auto self = shared_from_this();
        host->loop->post([self] { self->step(); });
    }

    void step() {
        size_t i = next++;
        if (i < libs->size()) {
            compileLib((*libs)[i]);
            postNext();
        } else if (i - libs->size() < pages->size()) {
            compilePage((*pages)[i - libs->size()]);   // posts the next step when it's done
        } else {
            if (!failed) out.bundle = makeBundlePtr(std::move(bundle));
            bundle.reset();
            auto finished = std::move(done);
            finished(std::move(out));
        }
    }

    void compileLib(const CodeFile& lib) {
        std::string fileName = codeFileName(lib.path());
        std::string_view name = lib.path().name();
        std::string importName(name.substr(0, name.size() - lib.path().extension().size()));
        if (!isIdentifier(importName)) {
            error({"can't be imported: the name before .ms must be a valid identifier", fileName, 0});
            return;
        }
        Value compileError;
        const std::string& source = lib.contents();
        FuncDef module = MiniScript::Interpreter::CompileToFunc(String(source.data(), source.size()),
                                                                String(fileName.c_str()), &compileError);
        if (!compileError.IsNull()) {
            error(scriptErrorFrom(compileError, fileName));
            return;
        }
        bundle->root(module);
        bundle->libs[importName] = module;
    }

    void compilePage(const CodeFile& file) {
        std::string fileName = codeFileName(file.path());
        Value compileError;
        List<FuncDef> functions = compileProgram(file.contents(), fileName, &compileError);
        if (!compileError.IsNull()) {
            error(scriptErrorFrom(compileError, fileName));
            postNext();
            return;
        }
        bundle->root(functions[0]);

        // Find the handlers by running the top level, as each request will,
        // in slices among the requests being served.
        auto run = std::make_unique<Run>();
        run->kind = Run::Kind::Discovery;
        run->host = host;
        run->fileName = fileName;
        run->libs = &bundle->libs;
        auto self = shared_from_this();
        RelPath path = file.path();
        run->discovered = [self, path, functions](Run& r, const HandlerOutcome& outcome) {
            if (outcome.kind == HandlerOutcome::Kind::Error) {
                self->error(outcome.error);
            } else {
                self->discover(r, path, functions);
            }
            self->postNext();
        };
        prepareInterpreter(*run, functions);
        host->schedule(std::move(run));
    }

    // Reads a page's handlers from the globals its top level left.
    void discover(Run& run, const RelPath& path, const List<FuncDef>& functions) {
        const std::string& fileName = run.fileName;
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

        if (pageOk) {
            out.pages[path] = handlers;
            bundle->pages.emplace(path, std::move(program));
        }
    }
};

}  // namespace

void ScriptHost::Impl::compile(const std::vector<CodeFile>& pages, const std::vector<CodeFile>& libs,
                               std::function<void(CompiledScripts)> done) {
    auto job = std::make_shared<CompileJob>();
    job->host = this;
    job->pages = &pages;
    job->libs = &libs;
    job->done = std::move(done);
    job->step();
}

}  // namespace fourfold

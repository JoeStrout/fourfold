// Compiling .ms files into a ScriptBundle, and finding each page's handlers;
// and evaluating declaration and config files as data.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <future>
#include <set>

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

// What a file's code refers to, found by scanning its tokens: whether it
// names the `session` global, and which libs it imports.
struct References {
    bool session = false;
    std::set<std::string> imports;
    bool dynamicImport = false;   // import of something other than a string literal
};

References scanReferences(const std::string& source) {
    References refs;
    size_t i = 0, n = source.size();
    char prevSignificant = 0;     // the last non-space character before this token
    bool afterImport = false;
    while (i < n) {
        char c = source[i];
        if (c == '/' && i + 1 < n && source[i + 1] == '/') {   // comment
            while (i < n && source[i] != '\n') i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }
        if (c == '"') {   // string; "" is a quote inside it
            i++;
            std::string text;
            while (i < n) {
                if (source[i] == '"') {
                    if (i + 1 < n && source[i + 1] == '"') {
                        text += '"';
                        i += 2;
                        continue;
                    }
                    break;
                }
                text += source[i++];
            }
            i++;
            if (afterImport) refs.imports.insert(text);
            afterImport = false;
            prevSignificant = '"';
            continue;
        }
        if (afterImport && c != '(') {
            refs.dynamicImport = true;
            afterImport = false;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80)) {
            size_t start = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_' ||
                             (static_cast<unsigned char>(source[i]) & 0x80))) {
                i++;
            }
            std::string word = source.substr(start, i - start);
            bool member = prevSignificant == '.';   // x.session isn't the global
            if (!member && word == "session") refs.session = true;
            afterImport = !member && word == "import";
            prevSignificant = 'a';
            continue;
        }
        prevSignificant = c;
        i++;
    }
    if (afterImport) refs.dynamicImport = true;
    return refs;
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
    std::map<std::string, References> libRefs;   // by import name
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
        libRefs[importName] = scanReferences(source);
    }

    // Whether code with these references can reach `session`,
    // directly or through the libs it imports (and theirs).
    bool usesSession(const References& refs) const {
        if (refs.session) return true;
        std::set<std::string> seen;
        std::vector<std::string> todo(refs.imports.begin(), refs.imports.end());
        if (refs.dynamicImport) {
            for (const auto& [name, r] : libRefs) todo.push_back(name);
        }
        while (!todo.empty()) {
            std::string name = todo.back();
            todo.pop_back();
            if (!seen.insert(name).second) continue;
            auto it = libRefs.find(name);
            if (it == libRefs.end()) continue;
            if (it->second.session) return true;
            todo.insert(todo.end(), it->second.imports.begin(), it->second.imports.end());
            if (it->second.dynamicImport) {
                for (const auto& [other, r] : libRefs) todo.push_back(other);
            }
        }
        return false;
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
        bool session = usesSession(scanReferences(file.contents()));
        run->discovered = [self, path, functions, session](Run& r, const HandlerOutcome& outcome) {
            if (outcome.kind == HandlerOutcome::Kind::Error) {
                self->error(outcome.error);
            } else {
                self->discover(r, path, functions, session);
            }
            self->postNext();
        };
        prepareInterpreter(*run, functions);
        host->schedule(std::move(run));
    }

    // Reads a page's handlers from the globals its top level left.
    void discover(Run& run, const RelPath& path, const List<FuncDef>& functions, bool usesSession) {
        const std::string& fileName = run.fileName;
        PageProgram program;
        program.fileName = fileName;
        program.functions = functions;
        PageHandlers handlers;
        handlers.usesSession = usesSession;
        bool pageOk = true;
        auto pageError = [&](const std::string& message) {
            error({message, fileName, 0});
            pageOk = false;
        };

        // access = "login" (overview §5): read now, at compile time. The top
        // level has no data, session or files here, so the declaration can't
        // depend on anything but the code (R10).
        Value access = run.interp.GetGlobalValue("access");
        if (!access.IsNull()) {
            std::string why;
            std::optional<Datum> d = toDatum(access, why);
            if (!d) {
                pageError("access must be \"login\", a role name, or a list of role names; it " + why);
            } else {
                handlers.access = std::move(*d);
            }
        }

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

namespace {

// What a data file's run may use: about a tenth of a request's budget. A
// declaration or config file only builds maps and lists.
constexpr uint64_t kDataInstructions = 1'000'000;

}  // namespace

void ScriptHost::Impl::evaluate(const std::string& source, const std::string& fileName, const std::string& global,
                                std::function<void(EvaluatedData)> done) {
    Value compileError;
    List<FuncDef> functions = compileProgram(source, fileName, &compileError);
    if (!compileError.IsNull()) {
        EvaluatedData out;
        out.error = scriptErrorFrom(compileError, fileName);
        done(std::move(out));
        return;
    }
    auto run = std::make_unique<Run>();
    run->kind = Run::Kind::Data;
    run->host = this;
    run->fileName = fileName;
    run->instructionLimit = kDataInstructions;
    // The functions are kept alive by the interpreter that runs them.
    run->discovered = [global, fileName, done](Run& r, const HandlerOutcome& outcome) {
        EvaluatedData out;
        if (outcome.kind == HandlerOutcome::Kind::Error) {
            out.error = outcome.error;
        } else {
            std::string why;
            Value v = r.interp.GetGlobalValue(String(global.c_str()));
            std::optional<Datum> d = toDatum(v, why);
            if (v.IsError()) {
                out.error = scriptErrorFrom(v, fileName);   // a failed call, like data.get here
            } else if (!d) {
                out.error = ScriptError{global + " " + why + " (only numbers, strings, lists and maps are allowed here)",
                                        fileName, 0};
            } else {
                out.value = std::move(*d);
            }
        }
        done(std::move(out));
    };
    prepareInterpreter(*run, functions);
    schedule(std::move(run));
}

namespace {

EvaluatedData evaluateOnScriptThread(ScriptHost::Impl* impl, std::string source, std::string fileName,
                                     std::string global) {
    auto finished = std::make_shared<std::promise<EvaluatedData>>();
    std::future<EvaluatedData> result = finished->get_future();
    impl->loop->post([impl, source = std::move(source), fileName, global = std::move(global), finished] {
        impl->evaluate(source, fileName, global, [finished](EvaluatedData out) { finished->set_value(std::move(out)); });
    });
    try {
        return result.get();
    } catch (const std::future_error&) {
        EvaluatedData out;
        out.error = ScriptError{"the script host stopped while evaluating", fileName, 0};
        return out;
    }
}

}  // namespace

EvaluatedData ScriptHost::evaluate(const CodeFile& file, const std::string& global) {
    return evaluateOnScriptThread(impl_.get(), file.contents(), codeFileName(file.path()), global);
}

EvaluatedData ScriptHost::evaluate(const ConfigFile& file, const std::string& global) {
    return evaluateOnScriptThread(impl_.get(), file.contents(), file.name(), global);
}

}  // namespace fourfold

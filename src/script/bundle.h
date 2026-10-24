// ScriptBundle: the compiled .ms files of one app generation.
//
// The bytecode's constants are MiniScript values, so they live in the GC and
// must be rooted while the bundle exists; a bundle is therefore created,
// used, and destroyed only on the script thread. Its shared_ptr deleter
// takes care of the last: whichever thread lets go last, the bundle is
// destroyed on the script thread (see makeBundlePtr).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "compiledapp/compiled_app.h"
#include "ms2.h"
#include "platform/event_loop.h"

namespace fourfold {

// A precompiled call to one handler: "_ff_result = get.more(_ff_req)".
struct HandlerStub {
    MiniScript::List<MiniScript::FuncDef> functions;
};

struct PageProgram {
    std::string fileName;   // "code/pages/hello.ms", as diagnostics and errors name it
    MiniScript::List<MiniScript::FuncDef> functions;   // [0] is @main
    std::map<std::string, HandlerStub> stubs;           // by HandlerRef::label()
};

class ScriptBundle {
public:
    ScriptBundle() = default;
    ~ScriptBundle();   // unroots everything rooted

    ScriptBundle(const ScriptBundle&) = delete;
    ScriptBundle& operator=(const ScriptBundle&) = delete;

    // Keeps `main`, and everything its constants reach, alive until the
    // bundle is destroyed.
    void root(MiniScript::FuncDef main);

    std::map<RelPath, PageProgram> pages;               // by "pages/hello.ms"
    std::map<std::string, MiniScript::FuncDef> libs;    // by import name ("util")

    const PageProgram* page(const RelPath& source) const;
    const HandlerStub* stub(const HandlerRef& handler) const;

private:
    std::vector<MiniScript::Value> roots_;
};

// Wraps a new bundle in a shared_ptr whose deleter destroys it on the script
// thread, if one is running.
std::shared_ptr<const ScriptBundle> makeBundlePtr(std::unique_ptr<ScriptBundle> bundle);

// Tells makeBundlePtr's deleters where the script thread is (null when no
// ScriptHost is running). Called by ScriptHost on its own thread.
void setBundleDisposalLoop(EventLoop* loop);

}  // namespace fourfold

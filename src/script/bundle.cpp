#include "bundle.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace fourfold {

namespace {

std::mutex g_disposalMutex;
EventLoop* g_disposalLoop = nullptr;
std::thread::id g_scriptThread;
std::atomic<int> g_liveBundles{0};

}  // namespace

ScriptBundle::ScriptBundle() {
    g_liveBundles++;
}

int ScriptBundle::liveCount() {
    return g_liveBundles;
}

ScriptBundle::~ScriptBundle() {
    g_liveBundles--;
    for (const MiniScript::Value& v : roots_) MiniScript::GCManager::RemoveRoot(v);
}

void ScriptBundle::root(MiniScript::FuncDef main) {
    MiniScript::Value ref = MiniScript::Value::make_funcref(main, MiniScript::Value::Null);
    MiniScript::GCManager::AddRoot(ref);
    roots_.push_back(ref);
}

const PageProgram* ScriptBundle::page(const RelPath& source) const {
    auto it = pages.find(source);
    return it == pages.end() ? nullptr : &it->second;
}

const HandlerStub* ScriptBundle::stub(const HandlerRef& handler) const {
    const PageProgram* p = page(handler.source);
    if (!p) return nullptr;
    auto it = p->stubs.find(handler.label());
    return it == p->stubs.end() ? nullptr : &it->second;
}

void setBundleDisposalLoop(EventLoop* loop) {
    std::lock_guard<std::mutex> lock(g_disposalMutex);
    g_disposalLoop = loop;
    g_scriptThread = loop ? std::this_thread::get_id() : std::thread::id();
}

std::shared_ptr<const ScriptBundle> makeBundlePtr(std::unique_ptr<ScriptBundle> bundle) {
    return std::shared_ptr<const ScriptBundle>(bundle.release(), [](const ScriptBundle* b) {
        {
            std::lock_guard<std::mutex> lock(g_disposalMutex);
            if (g_disposalLoop && std::this_thread::get_id() != g_scriptThread) {
                g_disposalLoop->post([b] { delete b; });
                return;
            }
        }
        // On the script thread, or with no script thread running (so nothing
        // else is touching MiniScript's state).
        delete b;
    });
}

}  // namespace fourfold

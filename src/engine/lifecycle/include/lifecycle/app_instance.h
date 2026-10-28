// AppInstance: one running app, recompiled as its code changes (Arch §5,
// lifecycle; overview §8 "Wait for quiet", §9).
//
// It watches code/ by polling: every pollInterval it lists the tree and
// compares each file's size and modification time with the listing it last
// compiled. Once the tree has differed, and stayed the same for
// quietPeriod, it compiles a new version while the old one keeps serving,
// and swaps it into the engine. A compile that fails keeps the old version.
// Touching code/.deploy compiles at once, without waiting for quiet.
//
// Compiles run on the instance's own thread, one at a time. The .ms files
// are compiled on the script thread, a file at a time, between requests.
//
// It also watches private/config/ (M6), if given a ConfigWatch: a changed
// config is read again, and if it's valid, swapped into the engine; if not,
// the previous one stays (overview §10). Later milestones add the
// drain–migrate–swap sequence here.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "base/result.h"
#include "compiledapp/compiled_app.h"
#include "config/app_config.h"
#include "observer/observer.h"
#include "partition/code_tree.h"
#include "pipeline/engine.h"
#include "platform/event_loop.h"
#include "script/script_host.h"

namespace fourfold {

// Watching the config (M6).
struct ConfigWatch {
    ConfigSources sources;     // sources.dir must outlive the instance
    std::string stamp;         // configStamp() of the config already in use
    // Called on the instance's thread after each reload, valid or not (a
    // valid one is already in the engine).
    std::function<void(const ConfigLoad&)> loaded;
};

struct LifecycleOptions {
    enum class Trigger {
        Quiet,        // compile once changes have stopped (dev); .deploy compiles at once
        DeployOnly,   // compile only when code/.deploy is touched
    };
    Trigger trigger = Trigger::Quiet;
    std::chrono::milliseconds pollInterval{250};
    // How long the tree must stay unchanged before compiling.
    std::chrono::milliseconds quietPeriod{200};
    // Reload private/config/ when it changes (needs a script host, which
    // evaluates it).
    std::optional<ConfigWatch> config;
};

class AppInstance {
public:
    // `scripts` may be null for an app without code; `observer` may be null.
    // The engine, script host and observer must outlive the instance.
    static Result<std::unique_ptr<AppInstance>> create(CodeTree code, ScriptHost* scripts, Engine& engine,
                                                       Observer* observer, LifecycleOptions options = {});
    // Stops watching, after any compile in progress.
    ~AppInstance();

    AppInstance(const AppInstance&) = delete;
    AppInstance& operator=(const AppInstance&) = delete;

    // The first compile, then watching. Waits for the compile, and returns
    // its report. Call once.
    CompileReport start();

    // Compiles now, whether or not anything changed, and waits for it.
    // Thread-safe (but not from an observer's compile events).
    CompileReport recompileNow();

    // The version serving now (the last good one), or null if none has
    // compiled yet.
    std::shared_ptr<const CompiledApp> current() const;

private:
    // A file's identity for change detection.
    struct Stamp {
        uint64_t size = 0;
        int64_t modifiedNs = 0;
        bool operator==(const Stamp& o) const { return size == o.size && modifiedNs == o.modifiedNs; }
        bool operator!=(const Stamp& o) const { return !(*this == o); }
    };
    using Snapshot = std::map<RelPath, Stamp>;

    AppInstance(CodeTree code, ScriptHost* scripts, Engine& engine, Observer* observer, LifecycleOptions options,
                std::unique_ptr<EventLoop> loop);

    // On the instance's thread.
    void schedulePoll();
    void poll();
    void pollConfig();
    CompileReport compileNow(const Snapshot& snapshot);
    std::optional<Snapshot> snapshot() const;

    CodeTree code_;
    ScriptHost* scripts_;
    Engine& engine_;
    Observer* observer_;
    LifecycleOptions options_;
    std::unique_ptr<EventLoop> loop_;
    std::thread thread_;

    // The instance's thread only.
    bool watching_ = false;
    bool compiledOnce_ = false;
    bool lastCompileOk_ = false;
    Snapshot compiled_;                       // as listed for the last compile
    std::optional<int64_t> deployTrigger_;    // as of the last compile
    std::optional<Snapshot> pending_;         // a changed tree, waiting for quiet
    EventLoop::Clock::time_point pendingSince_;
    uint64_t nextGeneration_ = 1;

    mutable std::mutex currentMutex_;
    std::shared_ptr<const CompiledApp> current_;
};

}  // namespace fourfold

#include "lifecycle/app_instance.h"

#include <algorithm>
#include <cctype>
#include <future>

#include "compiler/compiler.h"

namespace fourfold {

namespace {

bool isStylesheet(const RelPath& path) {
    std::string ext(path.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".css";
}

}  // namespace

Result<std::unique_ptr<AppInstance>> AppInstance::create(CodeTree code, ScriptHost* scripts, Engine& engine,
                                                         Observer* observer, LifecycleOptions options) {
    auto loop = EventLoop::create();
    if (!loop) return loop.error();
    return std::unique_ptr<AppInstance>(
        new AppInstance(std::move(code), scripts, engine, observer, options, std::move(loop).value()));
}

AppInstance::AppInstance(CodeTree code, ScriptHost* scripts, Engine& engine, Observer* observer,
                         LifecycleOptions options, std::unique_ptr<EventLoop> loop)
    : code_(std::move(code)),
      scripts_(scripts),
      engine_(engine),
      observer_(observer),
      options_(options),
      loop_(std::move(loop)) {
    thread_ = std::thread([this] { loop_->run(); });
}

AppInstance::~AppInstance() {
    loop_->stop();
    thread_.join();
}

CompileReport AppInstance::start() {
    CompileReport report = recompileNow();
    loop_->post([this] {
        watching_ = true;
        schedulePoll();
    });
    return report;
}

CompileReport AppInstance::recompileNow() {
    std::promise<CompileReport> done;
    loop_->post([this, &done] {
        auto listed = snapshot();
        done.set_value(compileNow(listed ? *listed : Snapshot{}));
    });
    return done.get_future().get();
}

std::shared_ptr<const CompiledApp> AppInstance::current() const {
    std::lock_guard<std::mutex> lock(currentMutex_);
    return current_;
}

std::optional<AppInstance::Snapshot> AppInstance::snapshot() const {
    auto listing = code_.list();
    if (!listing) return std::nullopt;
    Snapshot s;
    for (const CodeEntry& e : listing.value().files) s[e.path] = {e.size, e.modifiedNs};
    return s;
}

void AppInstance::schedulePoll() {
    loop_->addTimer(options_.pollInterval, [this] { poll(); });
}

void AppInstance::poll() {
    schedulePoll();

    std::optional<int64_t> trigger = code_.deployTrigger();
    if (trigger && trigger != deployTrigger_) {
        auto listed = snapshot();
        compileNow(listed ? *listed : compiled_);
        return;
    }
    if (options_.trigger == LifecycleOptions::Trigger::DeployOnly) return;

    // A listing that fails (e.g. too many files) is a compile error, and the
    // compiler will say why; treat it as a change only once.
    auto listed = snapshot();
    Snapshot now = listed ? std::move(*listed) : Snapshot{};
    if (now == compiled_) {
        pending_.reset();
        return;
    }
    auto t = EventLoop::Clock::now();
    if (!pending_ || *pending_ != now) {
        pending_ = std::move(now);   // still changing; wait for quiet
        pendingSince_ = t;
        return;
    }
    if (t - pendingSince_ >= options_.quietPeriod) compileNow(*pending_);
}

CompileReport AppInstance::compileNow(const Snapshot& snapshot) {
    if (observer_) observer_->compileStarted();
    auto t0 = std::chrono::steady_clock::now();
    CompileReport report;

    // What changed since the last compile (for live reload).
    if (compiledOnce_) {
        bool onlyStylesheets = lastCompileOk_;
        for (const auto& [path, stamp] : snapshot) {
            auto old = compiled_.find(path);
            if (old != compiled_.end() && old->second == stamp) continue;
            report.changed.push_back(path);
            if (old == compiled_.end() || !isStylesheet(path)) onlyStylesheets = false;
        }
        for (const auto& [path, stamp] : compiled_) {
            if (snapshot.count(path)) continue;
            report.changed.push_back(path);
            onlyStylesheets = false;
        }
        std::sort(report.changed.begin(), report.changed.end());
        report.cssOnly = onlyStylesheets && !report.changed.empty();
    }
    compiled_ = snapshot;
    deployTrigger_ = code_.deployTrigger();
    pending_.reset();
    compiledOnce_ = true;

    CompileOptions options;
    options.generation = nextGeneration_;
    options.scripts = scripts_;
    CompileOutput out = compile(code_, options);
    report.ok = out.ok();
    report.diagnostics = std::move(out.diagnostics);
    lastCompileOk_ = report.ok;
    if (report.ok) {
        nextGeneration_++;
        {
            std::lock_guard<std::mutex> lock(currentMutex_);
            current_ = out.app;
        }
        engine_.setApp(std::move(out.app));   // the old version goes when its requests finish
    } else {
        report.cssOnly = false;
    }
    if (auto app = current()) report.generation = app->generation;
    report.duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    if (observer_) observer_->compileFinished(report);
    return report;
}

}  // namespace fourfold

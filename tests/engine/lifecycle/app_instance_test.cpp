#include "lifecycle/app_instance.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <thread>

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {

// Collects compile reports, and lets a test wait for them.
struct Recorder : Observer {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<CompileReport> reports;
    int started = 0;

    void compileStarted() override {
        std::lock_guard<std::mutex> lock(mutex);
        started++;
    }
    void compileFinished(const CompileReport& report) override {
        std::lock_guard<std::mutex> lock(mutex);
        reports.push_back(report);
        changed.notify_all();
    }

    // Waits until there are `count` reports; returns the last, or nullopt.
    std::optional<CompileReport> waitFor(size_t count, milliseconds timeout = milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, timeout, [&] { return reports.size() >= count; })) return std::nullopt;
        return reports[count - 1];
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return reports.size();
    }
};

CodeTree codeTree(const TempDir& dir) {
    auto folder = AppFolder::open(dir.path(), AppFolder::Missing::Create);
    REQUIRE(folder.ok());
    auto code = folder.value().codeTree();
    REQUIRE(code.ok());
    return std::move(code).value();
}

std::vector<std::string> paths(const std::vector<RelPath>& list) {
    std::vector<std::string> result;
    for (const RelPath& p : list) result.push_back(p.str());
    return result;
}

LifecycleOptions fastOptions() {
    LifecycleOptions o;
    o.pollInterval = milliseconds(20);
    o.quietPeriod = milliseconds(60);
    return o;
}

// An app folder, a script host, an engine, and the instance under test.
struct Fixture {
    TempDir dir;
    Recorder recorder;
    std::unique_ptr<ScriptHost> scripts;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<AppInstance> instance;

    Fixture() {
        dir.write("code/pages/index.html", "<!DOCTYPE html><p>{{v}}</p>");
        dir.write("code/pages/index.ms", "render = function(req)\n  return {\"v\": 1}\nend function\n");
        dir.write("code/pages/style.css", "body { color: red }");
        auto host = ScriptHost::create();
        REQUIRE(host.ok());
        scripts = std::move(host).value();
    }

    ~Fixture() {
        instance.reset();
        scripts.reset();   // before the engine: runs in progress hold its options
    }

    void startInstance(LifecycleOptions options = fastOptions()) {
        engine = std::make_unique<Engine>(nullptr, codeTree(dir), scripts.get(), EngineOptions{localhostNames()});
        auto created = AppInstance::create(codeTree(dir), scripts.get(), *engine, &recorder, options);
        REQUIRE(created.ok());
        instance = std::move(created).value();
    }

    HttpResponse get(const std::string& target) {
        HttpRequest r;
        r.method = "GET";
        r.target = target;
        r.headers.add("Host", "localhost");
        auto promise = std::make_shared<std::promise<HttpResponse>>();
        auto future = promise->get_future();
        engine->handle(r, [promise](HttpResponse response) { promise->set_value(std::move(response)); });
        REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        return future.get();
    }

    // A change whose size differs, so it shows even with coarse timestamps.
    void edit(const std::string& rel, const std::string& contents) { dir.write("code/" + rel, contents); }
};

}  // namespace

TEST_CASE("AppInstance: start compiles, and serves the result") {
    Fixture f;
    f.startInstance();
    CompileReport r = f.instance->start();
    CHECK(r.ok);
    CHECK_EQ(r.generation, 1u);
    CHECK(r.changed.empty());
    CHECK_FALSE(r.cssOnly);
    REQUIRE(f.instance->current());
    CHECK_EQ(f.engine->app(), f.instance->current());
    CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>1</p>");
}

TEST_CASE("AppInstance: a change is compiled and swapped in") {
    Fixture f;
    f.startInstance();
    f.instance->start();
    f.edit("pages/index.ms", "render = function(req)\n  return {\"v\": 22}\nend function\n");
    auto r = f.recorder.waitFor(2);
    REQUIRE(r);
    CHECK(r->ok);
    CHECK_EQ(r->generation, 2u);
    CHECK_EQ(paths(r->changed), std::vector<std::string>{"pages/index.ms"});
    CHECK_FALSE(r->cssOnly);
    CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>22</p>");

    SUBCASE("added and removed files count as changes") {
        f.edit("pages/new.html", "<!DOCTYPE html><p>new</p>");
        REQUIRE(std::remove(f.dir.at("code/pages/style.css").c_str()) == 0);
        r = f.recorder.waitFor(3);
        REQUIRE(r);
        CHECK_EQ(paths(r->changed), (std::vector<std::string>{"pages/new.html", "pages/style.css"}));
        CHECK_EQ(f.get("/new").status, 200);
        CHECK_EQ(f.get("/style.css").status, 404);
    }
}

TEST_CASE("AppInstance: waits for changes to stop before compiling") {
    Fixture f;
    LifecycleOptions options = fastOptions();
    options.quietPeriod = milliseconds(150);
    f.startInstance(options);
    f.instance->start();
    auto until = Clock::now() + milliseconds(400);
    for (int i = 0; Clock::now() < until; i++) {
        f.edit("pages/index.html", "<!DOCTYPE html><p>{{v}} " + std::string(i, 'x') + "</p>");
        std::this_thread::sleep_for(milliseconds(30));
    }
    CHECK_EQ(f.recorder.count(), 1u);   // nothing while files kept changing
    auto r = f.recorder.waitFor(2);
    REQUIRE(r);
    CHECK(r->ok);
    std::this_thread::sleep_for(milliseconds(300));
    CHECK_EQ(f.recorder.count(), 2u);   // and once, after
}

TEST_CASE("AppInstance: code/.deploy compiles at once") {
    Fixture f;
    SUBCASE("in DeployOnly mode, nothing else does") {
        LifecycleOptions options = fastOptions();
        options.trigger = LifecycleOptions::Trigger::DeployOnly;
        f.startInstance(options);
        f.instance->start();
        f.edit("pages/index.html", "<!DOCTYPE html><p>v={{v}}</p>");
        std::this_thread::sleep_for(milliseconds(300));
        CHECK_EQ(f.recorder.count(), 1u);
        CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>1</p>");
        f.edit(".deploy", "");
        auto r = f.recorder.waitFor(2);
        REQUIRE(r);
        CHECK(r->ok);
        CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>v=1</p>");
    }
    SUBCASE("in Quiet mode, without waiting for quiet") {
        LifecycleOptions options = fastOptions();
        options.quietPeriod = milliseconds(5000);
        f.startInstance(options);
        f.instance->start();
        f.edit("pages/index.html", "<!DOCTYPE html><p>v={{v}}</p>");
        f.edit(".deploy", "");
        auto r = f.recorder.waitFor(2, milliseconds(1000));
        REQUIRE(r);
        CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>v=1</p>");
    }
}

TEST_CASE("AppInstance: a failed compile keeps the old version") {
    Fixture f;
    f.startInstance();
    f.instance->start();
    f.edit("pages/index.ms", "render = function(req)\n  return {\"v\": \nend function\n");
    auto r = f.recorder.waitFor(2);
    REQUIRE(r);
    CHECK_FALSE(r->ok);
    CHECK_EQ(r->generation, 1u);   // still serving
    REQUIRE_FALSE(r->diagnostics.empty());
    CHECK_EQ(r->diagnostics[0].file, "code/pages/index.ms");
    CHECK(r->diagnostics[0].line > 0);
    CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>1</p>");
    CHECK_EQ(f.instance->current()->generation, 1u);

    // No retries while nothing changes.
    std::this_thread::sleep_for(milliseconds(200));
    CHECK_EQ(f.recorder.count(), 2u);

    f.edit("pages/index.ms", "render = function(req)\n  return {\"v\": 3}\nend function\n");
    r = f.recorder.waitFor(3);
    REQUIRE(r);
    CHECK(r->ok);
    CHECK_EQ(r->generation, 2u);
    CHECK_FALSE(r->cssOnly);   // the page showed errors; it needs a reload
    CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>3</p>");
}

TEST_CASE("AppInstance: a first compile that fails serves 503 until fixed") {
    Fixture f;
    f.edit("pages/index.ms", "render = \n");
    f.startInstance();
    CompileReport r = f.instance->start();
    CHECK_FALSE(r.ok);
    CHECK_EQ(r.generation, 0u);
    CHECK_FALSE(f.instance->current());
    CHECK_EQ(f.get("/").status, 503);
    f.edit("pages/index.ms", "render = function(req)\n  return {\"v\": 4}\nend function\n");
    auto later = f.recorder.waitFor(2);
    REQUIRE(later);
    CHECK(later->ok);
    CHECK_EQ(later->generation, 1u);
    CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>4</p>");
}

TEST_CASE("AppInstance: stylesheet-only changes are marked cssOnly") {
    Fixture f;
    f.startInstance();
    f.instance->start();
    f.edit("pages/style.css", "body { color: blue }");
    auto r = f.recorder.waitFor(2);
    REQUIRE(r);
    CHECK(r->cssOnly);
    f.edit("pages/more.css", "p {}");   // a new stylesheet isn't linked from any open page yet
    r = f.recorder.waitFor(3);
    REQUIRE(r);
    CHECK_FALSE(r->cssOnly);
    f.edit("pages/style.css", "body { color: green }");
    f.edit("pages/index.html", "<!DOCTYPE html><p>{{v}}!</p>");
    r = f.recorder.waitFor(4);
    REQUIRE(r);
    CHECK_FALSE(r->cssOnly);
}

TEST_CASE("AppInstance: swapping repeatedly under load leaks no generations") {
    Fixture f;
    f.edit("pages/index.ms",
           "render = function(req)\n  return {\"v\": 0}\nend function\n"
           "get.slow = function(req)\n  wait 0.01\n  return \"slow\"\nend function\n");
    f.startInstance();
    REQUIRE(f.instance->start().ok);
    for (int i = 0; i < 5; i++) f.get("/");   // warm up
    ScriptHost::Stats baseline = f.scripts->stats(true);
    CHECK_EQ(baseline.liveBundles, 1);

    std::atomic<bool> stop{false};
    std::atomic<int> sent{0}, answered{0}, bad{0};
    std::vector<std::thread> clients;
    for (int c = 0; c < 4; c++) {
        clients.emplace_back([&, c] {
            while (!stop) {
                HttpRequest r;
                r.method = "GET";
                r.target = c % 2 ? "/slow" : "/";
                r.headers.add("Host", "localhost");
                sent++;
                std::promise<void> done;
                f.engine->handle(r, [&](HttpResponse response) {
                    bool ok = response.status == 200 &&
                              (response.body == "slow" || response.body.compare(0, 18, "<!DOCTYPE html><p>") == 0);
                    if (!ok) bad++;
                    answered++;
                    done.set_value();
                });
                done.get_future().wait();
            }
        });
    }

    int maxBundles = 0;
    for (int i = 1; i <= 200; i++) {
        f.edit("pages/index.ms",
               "render = function(req)\n  return {\"v\": " + std::to_string(i) + "}\nend function\n"
               "get.slow = function(req)\n  wait 0.01\n  return \"slow\"\nend function\n");
        REQUIRE(f.instance->recompileNow().ok);
        ScriptHost::Stats s = f.scripts->stats(true);   // a full collection between steps
        maxBundles = std::max(maxBundles, s.liveBundles);
    }
    stop = true;
    for (std::thread& t : clients) t.join();

    CHECK_EQ(bad.load(), 0);
    CHECK_EQ(sent.load(), answered.load());
    MESSAGE(answered.load(), " requests during 200 swaps; at most ", maxBundles, " generations live at once");
    CHECK(answered.load() > 200);
    CHECK(maxBundles <= 6);   // the current one, plus a few still finishing requests
    CHECK_EQ(f.get("/").body, "<!DOCTYPE html><p>200</p>");

    ScriptHost::Stats after = f.scripts->stats(true);
    CHECK_EQ(after.activeRuns, 0u);
    CHECK_EQ(after.liveBundles, 1);
    CHECK(after.liveMaps <= baseline.liveMaps + 5);
    CHECK(after.liveLists <= baseline.liveLists + 5);
    CHECK(after.liveFunctions <= baseline.liveFunctions + 5);
}

// bench_script: what a fresh VM per request costs (risk R1), measured through
// ScriptHost's public API, so it includes the hop to the script thread and
// back. Not a test; run it by hand from a release build:
//
//   ./build/release/bin/bench_script [runs]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <sys/resource.h>

#include "partition/app_folder.h"
#include "script/script_host.h"
#include "template/template.h"
#include "temp_dir.h"
#include "test_gate.h"

using namespace fourfold;
using SteadyClock = std::chrono::steady_clock;

namespace {

struct Bench {
    fourfold::test::TempDir dir;
    std::unique_ptr<ScriptHost> host;
    std::shared_ptr<CompiledApp> app = std::make_shared<CompiledApp>();

    Bench() {
        dir.write("code/lib/util.ms", "greet = function(name)\n  return \"Hello, \" + name\nend function\n");
        dir.write("code/pages/trivial.ms", "render = function(req)\n  return {\"name\": req.query.name}\nend function\n");
        dir.write("code/pages/typical.ms",
                  "import \"util\"\n"
                  "helper = function(x)\n  return x * 2\nend function\n"
                  "render = function(req)\n"
                  "  items = []\n"
                  "  for i in range(1, 20)\n    items.push(helper(i))\n  end for\n"
                  "  return {\"greeting\": util.greet(req.query.name), \"total\": items.sum}\n"
                  "end function\n");
        // Their pages, filled on the script thread as in a real request.
        dir.write("code/pages/trivial.html", "<!DOCTYPE html><p>Hello, {{name}}</p>");
        dir.write("code/pages/typical.html",
                  "<!DOCTYPE html><html><head><title>{{greeting}}</title><script src=\"/a.js\"></script></head>"
                  "<body><h1>{{greeting}}</h1><p>Total: {{total}}</p></body></html>");
        // Memory hogs: as much as each can allocate within the instruction budget.
        dir.write("code/pages/hogList.ms",
                  "render = function(req)\n  x = []\n  while true\n    x.push(x.len)\n  end while\nend function\n");
        dir.write("code/pages/hogString.ms",
                  "render = function(req)\n  parts = []\n  s = \"x\" * 1000000\n"
                  "  while true\n    parts.push(s + parts.len)\n  end while\nend function\n");
        ScriptHostOptions options;
        options.workerThreads = 2;
        auto h = ScriptHost::create(options);
        if (!h) {
            std::fprintf(stderr, "can't create ScriptHost: %s\n", h.error().message.c_str());
            std::exit(1);
        }
        host = std::move(h).value();
        auto folder = AppFolder::open(dir.path(), AppFolder::Missing::Create).value();
        auto code = folder.codeTree().value();
        std::vector<CodeFile> pages;
        for (const char* p : {"pages/trivial.ms", "pages/typical.ms", "pages/hogList.ms", "pages/hogString.ms"}) {
            pages.push_back(code.read(RelPath::parse(p).value()).value());
        }
        std::vector<CodeFile> libs{code.read(RelPath::parse("lib/util.ms").value()).value()};
        CompiledScripts compiled = host->compileScripts(pages, libs);
        if (!compiled.bundle) {
            for (const Diagnostic& d : compiled.diagnostics) std::fprintf(stderr, "%s\n", d.toString().c_str());
            std::exit(1);
        }
        app->scripts = compiled.bundle;
        for (const char* p : {"pages/trivial.html", "pages/typical.html"}) {
            RelPath path = RelPath::parse(p).value();
            ParsedTemplate t = parseTemplate(code.read(path).value(), TemplateKind::Page);
            if (!t.ok()) {
                std::fprintf(stderr, "%s: %s\n", p, t.errors[0].message.c_str());
                std::exit(1);
            }
            app->templates.add(path, std::move(t.compiled));
        }
    }

    HandlerRef render(const char* page) {
        return HandlerRef{HandlerRef::Kind::Render, RelPath::parse(page).value(), "render"};
    }

    RequestFacts request() {
        RequestFacts r;
        r.method = "GET";
        r.path = "/x";
        r.query = {{"name", "Ada"}};
        r.headers = {{"host", "localhost"}, {"user-agent", "bench"}};
        return r;
    }

    // One run at a time: the latency of a run, start to finish.
    double sequentialMicros(const char* page, int runs) {
        HandlerRef h = render(page);
        auto t0 = SteadyClock::now();
        for (int i = 0; i < runs; i++) {
            std::promise<void> done;
            host->run(app, h, TestGate::approve(), RunContext{request()}, [&](HandlerOutcome o) {
                if (o.kind != HandlerOutcome::Kind::Render) std::fprintf(stderr, "unexpected: %s\n", o.error.message.c_str());
                done.set_value();
            });
            done.get_future().wait();
        }
        return std::chrono::duration<double, std::micro>(SteadyClock::now() - t0).count() / runs;
    }

    // `inFlight` runs at a time: the script thread's throughput.
    double throughputPerSecond(const char* page, int runs, int inFlight) {
        HandlerRef h = render(page);
        std::mutex m;
        std::condition_variable cv;
        int started = 0, finished = 0;
        auto t0 = SteadyClock::now();
        std::function<void()> startOne = [&] {
            host->run(app, h, TestGate::approve(), RunContext{request()}, [&](HandlerOutcome) {
                std::lock_guard<std::mutex> lock(m);
                finished++;
                cv.notify_one();
            });
        };
        {
            std::unique_lock<std::mutex> lock(m);
            while (finished < runs) {
                while (started < runs && started - finished < inFlight) {
                    started++;
                    lock.unlock();
                    startOne();
                    lock.lock();
                }
                cv.wait(lock);
            }
        }
        double seconds = std::chrono::duration<double>(SteadyClock::now() - t0).count();
        return runs / seconds;
    }
};

}  // namespace

// Peak resident memory of this process so far, in MB (macOS reports bytes).
double maxRssMB() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return usage.ru_maxrss / (1024.0 * 1024.0);
#else
    return usage.ru_maxrss / 1024.0;
#endif
}

// Runs `count` copies of a memory hog at once; reports the peak memory.
void memoryHog(Bench& bench, const char* page, int count) {
    double before = maxRssMB();
    std::atomic<int> left{count};
    std::promise<void> done;
    std::string firstError;
    std::mutex m;
    for (int i = 0; i < count; i++) {
        bench.host->run(bench.app, bench.render(page), TestGate::approve(), RunContext{bench.request()}, [&](HandlerOutcome o) {
            {
                std::lock_guard<std::mutex> lock(m);
                if (firstError.empty()) firstError = o.error.message;
            }
            if (--left == 0) done.set_value();
        });
    }
    done.get_future().wait();
    std::printf("%-14s x%-4d peak RSS %7.0f MB (was %5.0f MB); ended with: %s\n", page + 6, count, maxRssMB(), before,
                firstError.c_str());
}

int main(int argc, char** argv) {
    int runs = argc > 1 ? std::atoi(argv[1]) : 20000;
    Bench bench;
    bench.sequentialMicros("pages/trivial.ms", 1000);   // warm up

    std::printf("fresh VM per run, %d runs each\n\n", runs);
    std::printf("%-10s %22s %18s %18s\n", "page", "latency (one at a time)", "1 in flight", "100 in flight");
    for (const char* page : {"pages/trivial.ms", "pages/typical.ms"}) {
        double latency = bench.sequentialMicros(page, runs);
        double t1 = bench.throughputPerSecond(page, runs, 1);
        double t100 = bench.throughputPerSecond(page, runs, 100);
        std::printf("%-10s %19.1f us %14.0f /s %14.0f /s\n", page + 6, latency, t1, t100);
    }
    std::printf("\nmemory (default budget: 10M instructions, wall clock 10 s):\n");
    memoryHog(bench, "pages/hogList.ms", 1);
    memoryHog(bench, "pages/hogString.ms", 1);
    memoryHog(bench, "pages/hogList.ms", 20);

    ScriptHost::Stats stats = bench.host->stats();
    std::printf("\nafter: %zu active runs; live maps %d, lists %d, functions %d\n", stats.activeRuns, stats.liveMaps,
                stats.liveLists, stats.liveFunctions);
    return 0;
}

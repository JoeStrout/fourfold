// bench_compile: what recompiling a large app costs the requests being served
// meanwhile (risk R5). The .ms files are compiled on the script thread, a
// file at a time, with requests in between; this measures how long that
// takes, and how much it delays a trivial request. Not a test; run it by
// hand from a release build:
//
//   ./build/release/bin/bench_compile [pages]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "compiler/compiler.h"
#include "partition/app_folder.h"
#include "script/script_host.h"
#include "temp_dir.h"
#include "test_gate.h"

using namespace fourfold;
using SteadyClock = std::chrono::steady_clock;

namespace {

// A page of about 100 lines: helpers, handlers, and a render.
std::string pageSource(int n) {
    std::string s = "import \"lib" + std::to_string(n % 20) + "\"\n";
    for (int f = 0; f < 8; f++) {
        s += "helper" + std::to_string(f) + " = function(items, x)\n"
             "  total = 0\n"
             "  for i in items\n"
             "    if i % 2 == 0 then\n"
             "      total = total + i * x\n"
             "    else\n"
             "      total = total - i\n"
             "    end if\n"
             "  end for\n"
             "  return total + " + std::to_string(f) + "\n"
             "end function\n";
    }
    s += "get.list = function(req)\n  return json([1, 2, 3])\nend function\n"
         "post.save = function(req)\n  return redirect(\"/p" + std::to_string(n) + "\")\nend function\n"
         "render = function(req)\n"
         "  items = range(1, 10)\n"
         "  return {\"title\": \"Page " + std::to_string(n) + "\", \"a\": helper0(items, 2), \"b\": helper7(items, 3)}\n"
         "end function\n";
    return s;
}

std::string libSource(int n) {
    std::string s;
    for (int f = 0; f < 5; f++) {
        s += "f" + std::to_string(f) + " = function(s)\n"
             "  parts = s.split(\" \")\n"
             "  out = []\n"
             "  for p in parts\n"
             "    out.push(p.upper)\n"
             "  end for\n"
             "  return out.join(\"-\") + \"" + std::to_string(n) + "\"\n"
             "end function\n";
    }
    return s;
}

struct Latencies {
    std::vector<double> us;

    void add(SteadyClock::duration d) { us.push_back(std::chrono::duration<double, std::micro>(d).count()); }
    double pct(double p) {
        if (us.empty()) return 0;
        std::sort(us.begin(), us.end());
        return us[std::min(us.size() - 1, static_cast<size_t>(p / 100.0 * static_cast<double>(us.size())))];
    }
    void print(const char* label) {
        std::printf("  %-22s %6zu requests   median %7.0f us   99th %7.0f us   max %7.0f us\n", label, us.size(),
                    pct(50), pct(99), pct(100));
    }
};

}  // namespace

int main(int argc, char** argv) {
    int pages = argc > 1 ? std::atoi(argv[1]) : 500;
    fourfold::test::TempDir dir;
    for (int i = 0; i < pages; i++) {
        dir.write("code/pages/p" + std::to_string(i) + ".ms", pageSource(i));
        dir.write("code/pages/p" + std::to_string(i) + ".html",
                  "<html><body><h1>{{title}}</h1><p>{{a}} and {{b}}</p></body></html>");
    }
    for (int i = 0; i < 20; i++) dir.write("code/lib/lib" + std::to_string(i) + ".ms", libSource(i));
    dir.write("code/pages/quick.ms", "render = function(req)\n  return \"quick\"\nend function\n");

    auto host = ScriptHost::create().value();
    auto folder = AppFolder::open(dir.path(), AppFolder::Missing::Create).value();
    auto code = folder.codeTree().value();

    auto compileApp = [&](uint64_t generation) {
        CompileOptions options;
        options.generation = generation;
        options.scripts = host.get();
        options.evaluator = host.get();
        return compile(code, options);
    };

    // A first compile, to warm up, and to have something to serve.
    CompileOutput first = compileApp(1);
    if (!first.ok()) {
        for (const Diagnostic& d : first.diagnostics) std::fprintf(stderr, "%s\n", d.toString().c_str());
        return 1;
    }
    std::shared_ptr<const CompiledApp> app = first.app;
    HandlerRef quick{HandlerRef::Kind::Render, RelPath::parse("pages/quick.ms").value(), "render"};

    auto one = [&] {
        std::promise<void> done;
        host->run(app, quick, TestGate::approve(), RunContext{}, [&](HandlerOutcome) { done.set_value(); });
        done.get_future().wait();
    };

    std::printf("An app of %d pages (each ~100 lines of .ms plus a template) and 20 libs\n\n", pages);

    // Requests with nothing else going on.
    Latencies idle;
    auto until = SteadyClock::now() + std::chrono::seconds(1);
    while (SteadyClock::now() < until) {
        auto t = SteadyClock::now();
        one();
        idle.add(SteadyClock::now() - t);
    }

    // The same, while the whole app recompiles.
    Latencies during;
    auto t0 = SteadyClock::now();
    auto compiling = std::async(std::launch::async, [&] { return compileApp(2); });
    while (compiling.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        auto t = SteadyClock::now();
        one();
        during.add(SteadyClock::now() - t);
    }
    double compileMs = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
    CompileOutput second = compiling.get();

    // And the compile alone, for comparison.
    auto t1 = SteadyClock::now();
    CompileOutput third = compileApp(3);
    double aloneMs = std::chrono::duration<double, std::milli>(SteadyClock::now() - t1).count();

    std::printf("Compile: %.0f ms with requests in between, %.0f ms alone (%s)\n", compileMs, aloneMs,
                second.ok() && third.ok() ? "ok" : "FAILED");
    std::printf("Trivial requests, one in flight at a time:\n");
    idle.print("idle");
    during.print("during the compile");
    return 0;
}

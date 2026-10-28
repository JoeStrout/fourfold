// bench_state: what sessions and data files cost a request (risk R7). Runs
// requests through the engine (no HTTP), with the real SQLite session store
// and data files, and compares pages that don't touch state with pages that
// do. Not a test; run it by hand from a release build:
//
//   ./build/release/bin/bench_state [requests]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string>

#include "appdata/data_files.h"
#include "compiler/compiler.h"
#include "partition/app_folder.h"
#include "pipeline/engine.h"
#include "policy/policy_gate.h"
#include "script/script_host.h"
#include "state/lock_service.h"
#include "state/session_store.h"
#include "temp_dir.h"

using namespace fourfold;
using SteadyClock = std::chrono::steady_clock;

namespace {

struct Bench {
    test::TempDir dir;
    std::unique_ptr<ScriptHost> host;
    std::unique_ptr<SessionStore> sessions;
    std::unique_ptr<LockService> locks = makeMemoryLockService();
    std::unique_ptr<DataFiles> data;
    std::unique_ptr<Engine> engine;
    std::string sessionCookie;   // "ff_session=..."

    Bench() {
        dir.write("code/pages/plain.ms", "render = function(req)\n  return {\"n\": 1}\nend function\n");
        dir.write("code/pages/plain.html", "<!DOCTYPE html><p>{{n}}</p>");
        dir.write("code/pages/reads.ms", "render = function(req)\n  return {\"n\": session.len}\nend function\n");
        dir.write("code/pages/reads.html", "<!DOCTYPE html><p>{{n}}</p>");
        dir.write("code/pages/writes.ms",
                  "render = function(req)\n"
                  "  if not session.hasIndex(\"n\") then session.n = 0\n"
                  "  session.n += 1\n"
                  "  return {\"n\": session.n}\n"
                  "end function\n");
        dir.write("code/pages/writes.html", "<!DOCTYPE html><p>{{n}}</p>");
        dir.write("code/pages/counter.ms", "render = function(req)\n  return {\"n\": data.increment(\"c\")}\nend function\n");
        dir.write("code/pages/counter.html", "<!DOCTYPE html><p>{{n}}</p>");
        dir.write("code/pages/getter.ms", "render = function(req)\n  return {\"n\": data.get(\"c\", 0)}\nend function\n");
        dir.write("code/pages/getter.html", "<!DOCTYPE html><p>{{n}}</p>");

        auto app = AppFolder::open(dir.path(), AppFolder::Missing::Create).value();
        host = ScriptHost::create().value();
        CompileOptions options;
        options.scripts = host.get();
        options.evaluator = host.get();
        CompileOutput out = compile(app.codeTree().value(), options);
        if (!out.ok()) {
            for (const auto& d : out.diagnostics) std::fprintf(stderr, "%s\n", d.toString().c_str());
            std::exit(1);
        }
        StateDir state = app.stateDir().value();
        sessions = SessionStore::open(state.sqlitePath(RelPath::parse("state.db").value()).value(), {}).value();
        data = std::make_unique<DataFiles>(app.dataDir().value());
        EngineOptions engineOptions;
        engineOptions.allowedHosts = localhostNames();
        engineOptions.sessions = sessions.get();
        engineOptions.sessionLocks = locks.get();
        engineOptions.data = data.get();
        engine = std::make_unique<Engine>(out.app, app.codeTree().value(), host.get(), engineOptions);

        // A session to use.
        HttpResponse first = send("/writes", "");
        for (const auto& h : first.headers) {
            if (h.name == "Set-Cookie" && h.value.compare(0, 11, "ff_session=") == 0) {
                sessionCookie = h.value.substr(0, h.value.find(';'));
            }
        }
        if (sessionCookie.empty()) {
            std::fprintf(stderr, "no session cookie\n");
            std::exit(1);
        }
    }

    HttpRequest request(const std::string& path, const std::string& cookie) const {
        HttpRequest r;
        r.method = "GET";
        r.target = path;
        r.headers.add("Host", "localhost");
        if (!cookie.empty()) r.headers.add("Cookie", cookie);
        r.peerAddress = "127.0.0.1";
        return r;
    }

    HttpResponse send(const std::string& path, const std::string& cookie) {
        std::promise<HttpResponse> done;
        engine->handle(request(path, cookie), [&](HttpResponse r) { done.set_value(std::move(r)); });
        return done.get_future().get();
    }

    // One request at a time: the latency of a request.
    double sequentialMicros(const std::string& path, const std::string& cookie, int runs) {
        auto t0 = SteadyClock::now();
        for (int i = 0; i < runs; i++) {
            HttpResponse r = send(path, cookie);
            if (r.status != 200) {
                std::fprintf(stderr, "%s: status %d: %s\n", path.c_str(), r.status, r.body.c_str());
                std::exit(1);
            }
        }
        return std::chrono::duration<double, std::micro>(SteadyClock::now() - t0).count() / runs;
    }

    // `inFlight` requests at a time: throughput, per second.
    double throughput(const std::string& path, const std::string& cookie, int runs, int inFlight) {
        std::mutex m;
        std::condition_variable cv;
        int started = 0, finished = 0;
        HttpRequest r = request(path, cookie);
        std::function<void()> startOne = [&] {
            engine->handle(r, [&](HttpResponse) {
                bool more;
                {
                    std::lock_guard<std::mutex> lock(m);
                    finished++;
                    more = started < runs;
                    if (more) started++;
                    cv.notify_one();
                }
                if (more) startOne();
            });
        };
        auto t0 = SteadyClock::now();
        {
            std::lock_guard<std::mutex> lock(m);
            started = std::min(inFlight, runs);
        }
        for (int i = 0; i < std::min(inFlight, runs); i++) startOne();
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return finished >= runs; });
        double seconds = std::chrono::duration<double>(SteadyClock::now() - t0).count();
        return runs / seconds;
    }
};

}  // namespace

int main(int argc, char** argv) {
    int runs = argc > 1 ? std::atoi(argv[1]) : 5000;
    Bench bench;
    bench.sequentialMicros("/plain", "", 500);   // warm up

    std::printf("%d requests each, through the engine (no HTTP)\n\n", runs);
    std::printf("%-44s %12s %14s %14s\n", "page", "latency", "1 in flight", "50 in flight");
    struct Row {
        const char* label;
        const char* path;
        bool withSession;
    };
    const Row rows[] = {
        {"plain render (no state)", "/plain", false},
        {"reads session (load + lock, no save)", "/reads", true},
        {"writes session (load + lock + save)", "/writes", true},
        {"data.get", "/getter", false},
        {"data.increment (lock + read + atomic write)", "/counter", false},
    };
    for (const Row& row : rows) {
        std::string cookie = row.withSession ? bench.sessionCookie : "";
        double latency = bench.sequentialMicros(row.path, cookie, runs);
        double t1 = bench.throughput(row.path, cookie, runs, 1);
        double t50 = bench.throughput(row.path, cookie, runs, 50);
        std::printf("%-44s %9.0f us %10.0f /s %10.0f /s\n", row.label, latency, t1, t50);
    }
    std::printf("\n(50 in flight on one session: they queue on its lock, one at a time.)\n");
    return 0;
}

// Jobs, code/schedule.ms, and push channels' access (M9.1; overview §2, §7).
#include "compiler/compiler.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

#include <filesystem>

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

CompileOutput compileApp(const std::string& root) {
    auto app = AppFolder::open(root, AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto tree = app.value().codeTree();
    REQUIRE(tree.ok());
    auto host = ScriptHost::create();
    REQUIRE(host.ok());
    CompileOptions options;
    options.scripts = host.value().get();
    options.evaluator = host.value().get();
    return compile(tree.value(), options);
}

std::string allDiagnostics(const CompileOutput& out) {
    std::string s;
    for (const Diagnostic& d : out.diagnostics) s += d.toString() + "\n";
    return s;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("jobs: code/jobs/ files' job maps, by name") {
    TempDir dir;
    dir.write("code/pages/index.html", "<!DOCTYPE html><html><body>hi</body></html>");
    dir.write("code/jobs/mail.ms", "job.sendWelcome = function(args)\n    return null\nend function\n"
                                   "job.digest = function\n    return null\nend function\n");
    dir.write("code/jobs/sub/clean.ms", "job.cleanup = function(args)\nend function\n");
    dir.write("code/schedule.ms", "schedule = {}\nschedule[\"cleanup\"] = \"daily 03:00\"\n"
                                  "schedule[\"digest\"] = \"every 15m\"\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const CompiledApp& app = *out.app;
    REQUIRE(app.jobs.size() == 3);
    CHECK(app.jobs.at("sendWelcome").source.str() == "jobs/mail.ms");
    CHECK(app.jobs.at("cleanup").label() == "job.cleanup");
    REQUIRE(app.schedule.size() == 2);
    CHECK(app.schedule[0].job == "cleanup");
    CHECK(app.schedule[1].schedule.everySeconds == 900);
    CHECK(contains(describe(app), "cleanup  jobs/sub/clean.ms  schedule: daily 03:00"));
    // No URL reaches a job.
    for (const auto& [url, entry] : app.routes) CHECK(url.find("jobs") == std::string::npos);
    CHECK(out.diagnostics.empty());
}

TEST_CASE("jobs: mistakes") {
    TempDir dir;
    dir.write("code/pages/index.html", "<!DOCTYPE html><html><body>hi</body></html>");
    // The same name in two files.
    dir.write("code/jobs/a.ms", "job.x = function(args)\nend function\n");
    dir.write("code/jobs/b.ms", "job.x = function(args)\nend function\n");
    CompileOutput out = compileApp(dir.path());
    CHECK(!out.ok());
    CHECK(contains(allDiagnostics(out), "job names must be unique"));

    std::filesystem::remove(dir.at("code/jobs/b.ms"));
    struct Bad {
        const char* file;
        const char* source;
        const char* message;
    };
    for (const Bad& b : std::vector<Bad>{
             {"code/jobs/b.ms", "job.y = 3\n", "job.y must be a function"},
             {"code/jobs/b.ms", "job.y = function(a, b)\nend function\n", "takes one parameter"},
             {"code/jobs/b.ms", "job = 1\n", "job must stay a map"},
             // A page's file has no `job` map.
             {"code/pages/p.ms", "job.y = function(args)\nend function\nget.z = function\nend function\n", "job"},
             {"code/schedule.ms", "schedule = {}\nschedule[\"nope\"] = \"daily 03:00\"\n", "there's no job named nope"},
             {"code/schedule.ms", "schedule = {}\nschedule[\"x\"] = \"now and then\"\n", "isn't a schedule"},
             {"code/schedule.ms", "schedule = {}\nschedule[\"x\"] = 15\n", "must be a string"},
             {"code/schedule.ms", "schedule = [1]\n", "must be a map"},
             {"code/pages/live/x.html", "<p>hi</p>", "/live/ is reserved for push channels"},
         }) {
        dir.write(b.file, b.source);
        out = compileApp(dir.path());
        CHECK_MESSAGE(!out.ok(), b.source);
        CHECK_MESSAGE(contains(allDiagnostics(out), b.message), b.source, "\n", allDiagnostics(out));
        std::filesystem::remove(dir.at(b.file));
    }
    out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
}

TEST_CASE("push channels: covered by access.ms rules, and only then") {
    TempDir dir;
    dir.write("code/pages/index.html", "<!DOCTYPE html><html><body>hi</body></html>");
    dir.write("code/access.ms", "rules = {}\nrules[\"/live\"] = \"public\"\nrules[\"/live/staff\"] = \"admin\"\n"
                                "rules[\"/live/members\"] = \"login\"\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    // Rules for /live cover no page, but aren't "unused": they're channels.
    CHECK(!contains(allDiagnostics(out), "covers no page"));
    const CompiledApp& app = *out.app;
    std::vector<std::string> sources;
    auto news = app.channelAccess("news", &sources);
    REQUIRE(news);
    CHECK(news->isPublic());
    CHECK(sources == std::vector<std::string>{"code/access.ms: /live"});
    auto staff = app.channelAccess("staff/room1");
    REQUIRE(staff);
    CHECK(staff->describe() == "admin");
    CHECK(app.channelAccess("members")->describe() == "login");
    CHECK(!app.channelAccess("bad name"));
    CHECK(!app.channelAccess("a//b"));
    // "public" changes no page: the index stays public.
    CHECK(app.routes.find("/")->access.isPublic());

    // Without a rule covering it, there's no channel.
    dir.write("code/access.ms", "rules = {}\nrules[\"/admin\"] = \"admin\"\n");
    dir.write("code/pages/admin.html", "<!DOCTYPE html><html><body>admin</body></html>");
    out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(!out.app->channelAccess("news"));
    // A rule for / covers every channel.
    dir.write("code/access.ms", "rules = {}\nrules[\"/\"] = \"login\"\n");
    out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(out.app->channelAccess("news")->describe() == "login");
}

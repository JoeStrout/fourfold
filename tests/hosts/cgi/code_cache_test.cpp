// CGI mode's compiled-app cache (M10; overview §8, Hosting modes).
#include "cgi/code_cache.h"

#include <filesystem>
#include <fstream>

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

RelPath rel(const char* s) { return RelPath::parse(s).value(); }

struct Fixture {
    TempDir dir;
    ManualClock clock;
    std::unique_ptr<ScriptHost> host = ScriptHost::create().value();
    std::optional<AppFolder> folder;
    std::optional<CodeTree> code;
    std::optional<StateDir> state;
    std::unique_ptr<CodeCache> cache;

    Fixture() {
        dir.write("code/pages/index.html", "<!DOCTYPE html><html><body>{{greeting}}</body></html>");
        dir.write("code/pages/index.ms", "import \"words\"\nrender = function(req)\n    return {\"greeting\": words.hi}\nend function\n");
        dir.write("code/pages/about.html", "<!DOCTYPE html><html><body>about</body></html>");
        dir.write("code/pages/hello.ms", "get.go = function\n    return \"hi\"\nend function\n");
        dir.write("code/lib/words.ms", "hi = \"hello\"\n");
        folder = AppFolder::open(dir.path(), AppFolder::Missing::Create).value();
        code = folder->codeTree().value();
        state = folder->stateDir().value();
        CodeCacheOptions options;
        options.clock = &clock;
        cache = std::make_unique<CodeCache>(*state, *code, *host, options);
    }

    CodeLoad load(const std::string& target = "/") {
        auto r = cache->load(target);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        return std::move(r).value();
    }

    bool exists(const std::string& path) { return std::filesystem::exists(dir.at(path)); }
};

std::string body(const CodeLoad& load, const char* path) {
    return load.code->read(rel(path)).value().contents();
}

}  // namespace

TEST_CASE("CodeCache: the first request compiles; later ones serve the version, compiling only their page's code") {
    Fixture f;
    CodeLoad first = f.load("/");
    CHECK(first.compiled);
    CHECK(first.generation == 1);
    REQUIRE(first.app->scripts.get() != nullptr);
    CHECK(f.exists("private/state/cgi/v1/code/pages/index.ms"));
    CHECK(f.exists("private/state/cgi/v1/version.json"));

    CodeLoad again = f.load("/?x=1");
    CHECK_FALSE(again.compiled);
    CHECK(again.generation == 1);
    CHECK(again.failed.empty());
    CHECK_EQ(describe(*again.app), describe(*first.app));
    CHECK(again.app->scripts.get() != nullptr);   // index.ms, and the lib it imports

    // A route that runs no code compiles none.
    CodeLoad about = f.load("/about");
    CHECK(about.app->scripts.get() == nullptr);
    CodeLoad handler = f.load("/hello/go");
    CHECK(handler.app->scripts.get() != nullptr);
    CHECK(f.cache->loadFull().value().app->scripts.get() != nullptr);
}

TEST_CASE("CodeCache: a change compiles once it has stayed the same for the quiet period") {
    Fixture f;
    f.load();
    f.dir.write("code/pages/about.html", "<!DOCTYPE html><html><body>about us</body></html>");
    CodeLoad seen = f.load();
    CHECK_FALSE(seen.compiled);
    CHECK(seen.generation == 1);
    CHECK_EQ(body(seen, "pages/about.html"), "<!DOCTYPE html><html><body>about</body></html>");   // the snapshot

    f.clock.advanceMillis(1000);
    CHECK(f.load().generation == 1);
    // Still changing: the quiet period starts again.
    f.dir.write("code/pages/about.html", "<!DOCTYPE html><html><body>about us!</body></html>");
    f.clock.advanceMillis(1500);
    CHECK(f.load().generation == 1);
    f.clock.advanceMillis(1500);
    CHECK(f.load().generation == 1);
    f.clock.advanceMillis(1000);
    CodeLoad next = f.load();
    CHECK(next.compiled);
    CHECK(next.generation == 2);
    CHECK_EQ(body(next, "pages/about.html"), "<!DOCTYPE html><html><body>about us!</body></html>");
    CHECK(f.load().generation == 2);
    CHECK_FALSE(f.load().compiled);
}

TEST_CASE("CodeCache: a version that doesn't compile is never installed, nor tried again") {
    Fixture f;
    f.load();
    f.dir.write("code/pages/index.ms", "render = function(req\n");
    f.load();
    f.clock.advanceMillis(3000);
    CodeLoad failed = f.load();
    CHECK_FALSE(failed.compiled);
    CHECK(failed.generation == 1);
    REQUIRE_FALSE(failed.failed.empty());
    CHECK(failed.failed[0].file == "code/pages/index.ms");
    CHECK_FALSE(f.exists("private/state/cgi/v2"));
    // The old version serves its own code, including the page that broke.
    CHECK(f.load("/").app->scripts.get() != nullptr);
    f.clock.advanceMillis(3000);
    CHECK(f.load().failed.empty());   // not tried again

    // Fixed: compiles (after quiet).
    f.dir.write("code/pages/index.ms", "render = function(req)\n    return {\"greeting\": \"fixed\"}\nend function\n");
    f.load();
    f.clock.advanceMillis(3000);
    CodeLoad fixed = f.load();
    CHECK(fixed.compiled);
    CHECK(fixed.generation == 3);
}

TEST_CASE("CodeCache: touching code/.deploy compiles at once") {
    Fixture f;
    f.load();
    f.dir.write("code/pages/about.html", "<!DOCTYPE html><html><body>new</body></html>");
    f.dir.write("code/.deploy", "");
    CodeLoad next = f.load();
    CHECK(next.compiled);
    CHECK(next.generation == 2);
}

TEST_CASE("CodeCache: an app that has never compiled is an error, until it's fixed") {
    Fixture f;
    f.dir.write("code/pages/index.ms", "render = function(req\n");
    auto r = f.cache->load("/");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("code/pages/index.ms") != std::string::npos);
    CHECK_FALSE(f.cache->load("/").ok());
    f.dir.write("code/pages/index.ms", "render = function(req)\n    return {\"greeting\": 1}\nend function\n");
    CodeLoad fixed = f.load();   // no version to protect: no waiting for quiet
    CHECK(fixed.compiled);
}

TEST_CASE("CodeCache: replaced versions are removed once they've been retired long enough") {
    Fixture f;
    f.load();
    f.dir.write("code/.deploy", "1");
    CHECK(f.load().generation == 2);
    CHECK(f.exists("private/state/cgi/v1"));
    f.clock.advanceSeconds(11 * 60);
    std::filesystem::last_write_time(f.dir.at("code/.deploy"),
                                     std::filesystem::last_write_time(f.dir.at("code/.deploy")) + std::chrono::seconds(5));
    CHECK(f.load().generation == 3);
    CHECK_FALSE(f.exists("private/state/cgi/v1"));
    CHECK(f.exists("private/state/cgi/v2"));
    CHECK(f.exists("private/state/cgi/v3"));
}

TEST_CASE("CodeCache: handlers round-trip through version.json's form") {
    PageHandlers h;
    h.render = true;
    h.get = {"a", "b"};
    h.post = {"sign"};
    h.webhook = {"hook"};
    h.usesSession = true;
    h.access = Datum::list({Datum::string("admin")});
    h.forms.emplace("sign", Datum::map({{"name", Datum::map({{"type", Datum::string("text")}})}}));
    auto back = handlersFromDatum(handlersToDatum(h));
    REQUIRE(back);
    CHECK(back->render);
    CHECK(back->get == h.get);
    CHECK(back->post == h.post);
    CHECK(back->webhook == h.webhook);
    CHECK(back->job.empty());
    CHECK(back->usesSession);
    CHECK(*back->access == *h.access);
    CHECK(back->forms.at("sign") == h.forms.at("sign"));
    CHECK_FALSE(handlersFromDatum(Datum::string("x")));
}

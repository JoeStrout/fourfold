// CGI mode's partial compiles (M10; overview §8): only the requested page's
// .ms file and the libs it imports, with every other file's handlers known
// from a full compile of the same code.
#include "compiler/compiler.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

RelPath rel(const char* s) { return RelPath::parse(s).value(); }

struct Compiler {
    std::unique_ptr<ScriptHost> host = ScriptHost::create().value();

    CompileOutput run(const std::string& root, const std::map<RelPath, PageHandlers>* known = nullptr,
                      std::set<RelPath> only = {}) {
        auto app = AppFolder::open(root, AppFolder::Missing::Create);
        REQUIRE(app.ok());
        CompileOptions options;
        options.scripts = host.get();
        options.evaluator = host.get();
        options.knownHandlers = known;
        options.compileOnly = std::move(only);
        return compile(app.value().codeTree().value(), options);
    }
};

void writeApp(TempDir& dir) {
    dir.write("code/pages/index.html", "<!DOCTYPE html><html><body>{{greeting}}</body></html>");
    dir.write("code/pages/index.ms", "import \"words\"\nrender = function(req)\n    return {\"greeting\": words.hi}\nend function\n");
    dir.write("code/pages/guestbook.ms", "access = \"login\"\nform.sign = {\"name\": {\"type\": \"text\", \"required\": true}}\n"
                                        "post.sign = function(req)\n    session.x = 1\n    return \"ok\"\nend function\n");
    dir.write("code/pages/about.md", "# About\n\nHi.\n");
    dir.write("code/lib/words.ms", "import \"deep\"\nhi = deep.word\n");
    dir.write("code/lib/deep.ms", "word = \"hello\"\n");
    dir.write("code/lib/unused.ms", "this is not MiniScript (\n");   // never compiled in partial mode
    dir.write("code/jobs/mail.ms", "job.send = function(args)\nend function\n");
}

}  // namespace

TEST_CASE("partial compile: the same app, with only the requested file's code") {
    TempDir dir;
    writeApp(dir);
    dir.write("code/lib/unused.ms", "x = 1\n");
    Compiler c;
    CompileOutput full = c.run(dir.path());
    REQUIRE(full.ok());
    REQUIRE(full.handlers.size() == 3);
    CHECK(full.handlers.at(rel("pages/guestbook.ms")).usesSession);
    CHECK(full.handlers.at(rel("jobs/mail.ms")).job == std::vector<std::string>{"send"});

    // Now the unused lib is broken: a partial compile never looks at it.
    dir.write("code/lib/unused.ms", "this is not MiniScript (\n");
    CompileOutput part = c.run(dir.path(), &full.handlers, {rel("pages/index.ms")});
    REQUIRE(part.ok());
    CHECK_EQ(describe(*part.app), describe(*full.app));
    REQUIRE(part.app->routes.find("/guestbook/sign"));
    CHECK(part.app->routes.find("/guestbook/sign")->access.describe() == "login");
    CHECK(part.app->forms.count("pages/guestbook.ms#sign") == 1);
    CHECK(part.app->jobs.count("send") == 1);
    REQUIRE(part.app->scripts.get() != nullptr);

    // No code at all, for a request that runs none.
    CompileOutput none = c.run(dir.path(), &full.handlers, {});
    REQUIRE(none.ok());
    CHECK(none.app->scripts.get() == nullptr);
    CHECK_EQ(describe(*none.app), describe(*full.app));
}

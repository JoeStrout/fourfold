#include <cstdlib>
#include <fstream>
#include <sstream>

#include "compiler/compiler.h"
#include "compiler/content_types.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

CompileOutput compileApp(const std::string& root, AppFolder::Missing missing = AppFolder::Missing::Create) {
    auto app = AppFolder::open(root, missing);
    REQUIRE(app.ok());
    auto tree = app.value().codeTree();
    REQUIRE(tree.ok());
    auto host = ScriptHost::create();
    REQUIRE(host.ok());
    CompileOptions options;
    options.scripts = host.value().get();
    return compile(tree.value(), options);
}

std::string allDiagnostics(const CompileOutput& out) {
    std::string s;
    for (const Diagnostic& d : out.diagnostics) s += d.toString() + "\n";
    return s;
}

const RouteEntry* route(const CompileOutput& out, const std::string& url) {
    REQUIRE(out.ok());
    return out.app->routes.find(url);
}

bool hasDiagnostic(const CompileOutput& out, const std::string& text) {
    for (const Diagnostic& d : out.diagnostics) {
        if (d.toString().find(text) != std::string::npos) return true;
    }
    return false;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("compile: pages map to clean URLs") {
    TempDir dir;
    dir.write("code/pages/index.html", "home");
    dir.write("code/pages/about.html", "about");
    dir.write("code/pages/docs/index.html", "docs");
    dir.write("code/pages/docs/intro.html", "intro");
    dir.write("code/pages/Caps.HTML", "caps");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(out.diagnostics.empty());

    struct Expect {
        const char* url;
        const char* source;
    };
    for (const Expect& e : {Expect{"/", "pages/index.html"}, Expect{"/about", "pages/about.html"},
                            Expect{"/docs", "pages/docs/index.html"},
                            Expect{"/docs/intro", "pages/docs/intro.html"}, Expect{"/Caps", "pages/Caps.HTML"}}) {
        CAPTURE(e.url);
        const RouteEntry* r = route(out, e.url);
        REQUIRE(r != nullptr);
        CHECK(r->kind == RouteKind::Page);
        CHECK_EQ(r->source.str(), e.source);
        CHECK_EQ(r->trailingSlash, std::string(e.url) == "/docs");   // only folder index pages
    }
    CHECK_EQ(out.app->routes.size(), 5u);
    CHECK(route(out, "/index") == nullptr);        // one URL per page
    CHECK(route(out, "/about.html") == nullptr);
    CHECK(route(out, "/caps") == nullptr);         // case matters
}

TEST_CASE("compile: static files keep their names and get a content type") {
    TempDir dir;
    dir.write("code/pages/style.css", "body{}");
    dir.write("code/pages/img/Logo.PNG", "png");
    CompileOutput out = compileApp(dir.path());
    const RouteEntry* css = route(out, "/style.css");
    REQUIRE(css != nullptr);
    CHECK(css->kind == RouteKind::Static);
    const StaticFile* f = out.app->statics.find(css->source);
    REQUIRE(f != nullptr);
    CHECK_EQ(f->contentType, "text/css; charset=utf-8");
    CHECK_EQ(f->size, 6u);

    const RouteEntry* png = route(out, "/img/Logo.PNG");
    REQUIRE(png != nullptr);
    CHECK_EQ(out.app->statics.find(png->source)->contentType, "image/png");
}

TEST_CASE("compile: 404.html is the not-found page, not a route") {
    TempDir dir;
    dir.write("code/pages/404.html", "missing");
    dir.write("code/pages/docs/404.html", "a page that happens to be called 404");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    REQUIRE(out.app->notFoundPage.has_value());
    CHECK_EQ(out.app->notFoundPage->str(), "pages/404.html");
    CHECK(route(out, "/404") == nullptr);
    CHECK(route(out, "/docs/404") != nullptr);
}

TEST_CASE("compile: two files for one URL is an error") {
    TempDir dir;
    dir.write("code/pages/docs.html", "one");
    dir.write("code/pages/docs/index.html", "two");
    CompileOutput out = compileApp(dir.path());
    CHECK_FALSE(out.ok());
    CHECK(hasDiagnostic(out, "error: URL /docs is also served by code/pages/doc"));
}

TEST_CASE("compile: reserved URL prefixes are errors") {
    for (const char* reserved : {"__auth", "__fourfold", "__dev"}) {
        CAPTURE(reserved);
        TempDir dir;
        dir.write(std::string("code/pages/") + reserved + "/x.html", "x");
        CompileOutput out = compileApp(dir.path());
        CHECK_FALSE(out.ok());
        CHECK(hasDiagnostic(out, "is reserved for Fourfold's own pages"));
    }
}

TEST_CASE("compile: warnings don't stop the build") {
    TempDir dir;
    dir.write("code/pages/index.html", "home");
    dir.write("code/pages/notes.xyz", "?");
    dir.write("code/pages/README", "?");
    dir.write("code/pages/index.ms", "x = 1");
    dir.write("code/pages/about.md", "# About");
    dir.write("code/real.css", "x");
    dir.symlink("../real.css", "code/pages/linked.css");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(hasDiagnostic(out, "code/pages/notes.xyz: warning: unknown file type .xyz; not served"));
    CHECK(hasDiagnostic(out, "code/pages/README: warning: file has no extension"));
    CHECK(hasDiagnostic(out, "code/pages/about.md: warning: Markdown pages aren't supported yet"));
    CHECK(hasDiagnostic(out, "code/pages/linked.css: warning: symbolic link (not followed); ignored"));
    CHECK_EQ(out.app->diagnostics.size(), out.diagnostics.size());
    CHECK_EQ(out.app->routes.size(), 1u);
}

TEST_CASE("compile: an app with no pages warns") {
    TempDir dir;
    dir.write("code/lib/util.ms", "x = 1");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(hasDiagnostic(out, "no pages found"));
}

TEST_CASE("contentTypeForExtension") {
    CHECK_EQ(std::string(contentTypeForExtension(".png")), "image/png");
    CHECK_EQ(std::string(contentTypeForExtension(".JPG")), "image/jpeg");
    CHECK_EQ(std::string(contentTypeForExtension(".js")), "text/javascript; charset=utf-8");
    CHECK(contentTypeForExtension(".exe") == nullptr);
    CHECK(contentTypeForExtension(".ms") == nullptr);
    CHECK(contentTypeForExtension("") == nullptr);
}

TEST_CASE("compile: a page's .ms file supplies its render") {
    TempDir dir;
    dir.write("code/pages/hello.html", "<p>{{name}}</p>");
    dir.write("code/pages/hello.ms", "render = function(req)\n  return {\"name\": \"x\"}\nend function\n");
    dir.write("code/pages/plain.html", "<p>{{unfilled}}</p>");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const RouteEntry* hello = route(out, "/hello");
    REQUIRE(hello);
    CHECK(hello->kind == RouteKind::Page);
    CHECK_EQ(hello->source.str(), "pages/hello.html");
    REQUIRE(hello->render);
    CHECK_EQ(hello->render->source.str(), "pages/hello.ms");
    const RouteEntry* plain = route(out, "/plain");
    REQUIRE(plain);
    CHECK_FALSE(plain->render);
    REQUIRE(out.app->templates.find(plain->source));
    CHECK(out.app->scripts != nullptr);
}

TEST_CASE("compile: get and post handlers get URLs below their page") {
    TempDir dir;
    dir.write("code/pages/guestbook.html", "<p>gb</p>");
    dir.write("code/pages/guestbook.ms",
              "get.entries = function(req)\n  return \"\"\nend function\n"
              "post.entries = function(req)\n  return \"\"\nend function\n"
              "post.sign = function(req)\n  return \"\"\nend function\n");
    dir.write("code/pages/docs/index.ms", "get.more = function(req)\n  return \"\"\nend function\n");
    dir.write("code/pages/index.ms", "get.ping = function(req)\n  return \"\"\nend function\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));

    const RouteEntry* entries = route(out, "/guestbook/entries");
    REQUIRE(entries);
    CHECK(entries->kind == RouteKind::Handler);
    REQUIRE(entries->get);
    REQUIRE(entries->post);
    CHECK_EQ(entries->get->label(), "get.entries");
    CHECK_EQ(entries->post->label(), "post.entries");
    const RouteEntry* sign = route(out, "/guestbook/sign");
    REQUIRE(sign);
    CHECK_FALSE(sign->get);
    CHECK(sign->post);
    CHECK(route(out, "/docs/more"));
    CHECK(route(out, "/ping"));
    CHECK(route(out, "/guestbook"));
}

TEST_CASE("compile: a .ms file with render and no .html is a page of its own") {
    TempDir dir;
    dir.write("code/pages/api.ms", "render = function(req)\n  return json(1)\nend function\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const RouteEntry* api = route(out, "/api");
    REQUIRE(api);
    CHECK(api->kind == RouteKind::Page);
    CHECK_EQ(api->source.str(), "pages/api.ms");
    CHECK(api->render);
}

TEST_CASE("compile: a handler and a page at the same URL collide") {
    TempDir dir;
    dir.write("code/pages/guestbook.ms", "get.more = function(req)\n  return \"\"\nend function\n");
    dir.write("code/pages/guestbook/more.html", "<p>more</p>");
    CompileOutput out = compileApp(dir.path());
    CHECK_FALSE(out.ok());
    CHECK(hasDiagnostic(out, "URL /guestbook/more is also served by"));
}

TEST_CASE("compile: template and script errors stop the build, with their lines") {
    TempDir dir;
    dir.write("code/pages/a.html", "<p>\n<a href=\"{{url}}\">x</a></p>");
    dir.write("code/pages/b.ms", "render = function(req)\n  return (1 +\nend function\n");
    CompileOutput out = compileApp(dir.path());
    CHECK_FALSE(out.ok());
    CHECK(hasDiagnostic(out, "code/pages/a.html:2: error: a field isn't allowed inside a tag"));
    CHECK(hasDiagnostic(out, "code/pages/b.ms:"));
}

TEST_CASE("compile: .ms files outside pages/ and lib/ are ignored, with a warning") {
    TempDir dir;
    dir.write("code/pages/index.html", "home");
    dir.write("code/stray.ms", "x = 1");
    dir.write("code/lib/sub/deep.ms", "x = 1");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    CHECK(hasDiagnostic(out, "code/stray.ms: warning: MiniScript files belong in"));
    CHECK(hasDiagnostic(out, "code/lib/sub/deep.ms: warning: can't be imported"));
}

TEST_CASE("compile: MiniScript files need a script compiler") {
    TempDir dir;
    dir.write("code/pages/index.ms", "x = 1");
    auto app = AppFolder::open(dir.path(), AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto tree = app.value().codeTree();
    REQUIRE(tree.ok());
    CompileOutput out = compile(tree.value());
    CHECK_FALSE(out.ok());
    CHECK(hasDiagnostic(out, "no script compiler"));
}

TEST_CASE("compile: the 404 and 500 pages aren't routes") {
    TempDir dir;
    dir.write("code/pages/404.html", "missing");
    dir.write("code/pages/500.html", "broken");
    dir.write("code/pages/404.ms", "render = function(req)\n  return {}\nend function\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    CHECK_EQ(out.app->routes.size(), 0u);
    CHECK_EQ(out.app->notFoundPage->str(), "pages/404.html");
    CHECK_EQ(out.app->errorPage->str(), "pages/500.html");
    CHECK(hasDiagnostic(out, "code/pages/404.ms: warning: the 404 page can't have code yet"));
}

namespace {

// Golden tests: compile an app in tests/apps/ and compare its full
// description with tests/compiler/golden/<name>.txt. To accept a change on
// purpose, run with FOURFOLD_UPDATE_GOLDEN=1 and review the diff.
void checkGolden(const std::string& name) {
    CompileOutput out = compileApp(FOURFOLD_TEST_APPS_DIR "/" + name, AppFolder::Missing::Fail);
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    std::string actual = describe(*out.app);
    std::string goldenPath = FOURFOLD_TESTS_DIR "/compiler/golden/" + name + ".txt";
    if (std::getenv("FOURFOLD_UPDATE_GOLDEN")) {
        std::ofstream(goldenPath, std::ios::binary) << actual;
        MESSAGE("updated " << goldenPath);
    }
    CHECK_EQ(actual, readFile(goldenPath));
}

}  // namespace

TEST_CASE("compile: golden route table for tests/apps/static-site") {
    checkGolden("static-site");
}

TEST_CASE("compile: golden route table for tests/apps/scripts") {
    checkGolden("scripts");
}

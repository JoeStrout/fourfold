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
    return compile(tree.value());
}

const RouteEntry* route(const CompileOutput& out, const std::string& url) {
    REQUIRE(out.ok());
    return out.image->routes.find(url);
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
    CHECK_EQ(out.image->routes.size(), 5u);
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
    const StaticFile* f = out.image->statics.find(css->source);
    REQUIRE(f != nullptr);
    CHECK_EQ(f->contentType, "text/css; charset=utf-8");
    CHECK_EQ(f->size, 6u);

    const RouteEntry* png = route(out, "/img/Logo.PNG");
    REQUIRE(png != nullptr);
    CHECK_EQ(out.image->statics.find(png->source)->contentType, "image/png");
}

TEST_CASE("compile: 404.html is the not-found page, not a route") {
    TempDir dir;
    dir.write("code/pages/404.html", "missing");
    dir.write("code/pages/docs/404.html", "a page that happens to be called 404");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    REQUIRE(out.image->notFoundPage.has_value());
    CHECK_EQ(out.image->notFoundPage->str(), "pages/404.html");
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
    dir.write("code/real.css", "x");
    dir.symlink("../real.css", "code/pages/linked.css");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(hasDiagnostic(out, "code/pages/notes.xyz: warning: unknown file type .xyz; not served"));
    CHECK(hasDiagnostic(out, "code/pages/README: warning: file has no extension"));
    CHECK(hasDiagnostic(out, "code/pages/index.ms: warning: MiniScript and Markdown pages"));
    CHECK(hasDiagnostic(out, "code/pages/linked.css: warning: symbolic link (not followed); ignored"));
    CHECK_EQ(out.image->diagnostics.size(), out.diagnostics.size());
    CHECK_EQ(out.image->routes.size(), 1u);
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

// The golden test: compile tests/apps/static-site and compare its full
// description with tests/compiler/golden/static-site.txt. To accept a change
// on purpose, run with FOURFOLD_UPDATE_GOLDEN=1 and review the diff.
TEST_CASE("compile: golden route table for tests/apps/static-site") {
    CompileOutput out = compileApp(FOURFOLD_TEST_APPS_DIR "/static-site", AppFolder::Missing::Fail);
    REQUIRE(out.ok());
    std::string actual = describe(*out.image);
    std::string goldenPath = FOURFOLD_TESTS_DIR "/compiler/golden/static-site.txt";
    if (std::getenv("FOURFOLD_UPDATE_GOLDEN")) {
        std::ofstream(goldenPath, std::ios::binary) << actual;
        MESSAGE("updated " << goldenPath);
    }
    CHECK_EQ(actual, readFile(goldenPath));
}

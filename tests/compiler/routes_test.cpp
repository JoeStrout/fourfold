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

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
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
    dir.write("code/pages/index.html", "<!DOCTYPE html>home");
    dir.write("code/pages/about.html", "<!DOCTYPE html>about");
    dir.write("code/pages/docs/index.html", "<!DOCTYPE html>docs");
    dir.write("code/pages/docs/intro.html", "<!DOCTYPE html>intro");
    dir.write("code/pages/Caps.HTML", "<!DOCTYPE html>caps");
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
    dir.write("code/pages/404.html", "<!DOCTYPE html>missing");
    dir.write("code/pages/docs/404.html", "<!DOCTYPE html>a page that happens to be called 404");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    REQUIRE(out.app->notFoundPage.has_value());
    CHECK_EQ(out.app->notFoundPage->str(), "pages/404.html");
    CHECK(route(out, "/404") == nullptr);
    CHECK(route(out, "/docs/404") != nullptr);
}

TEST_CASE("compile: two files for one URL is an error") {
    TempDir dir;
    dir.write("code/pages/docs.html", "<!DOCTYPE html>one");
    dir.write("code/pages/docs/index.html", "<!DOCTYPE html>two");
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
    dir.write("code/pages/index.html", "<!DOCTYPE html>home");
    dir.write("code/pages/notes.xyz", "?");
    dir.write("code/pages/README", "?");
    dir.write("code/pages/index.ms", "x = 1");
    dir.write("code/real.css", "x");
    dir.symlink("../real.css", "code/pages/linked.css");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(hasDiagnostic(out, "code/pages/notes.xyz: warning: unknown file type .xyz; not served"));
    CHECK(hasDiagnostic(out, "code/pages/README: warning: file has no extension"));
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
    dir.write("code/pages/hello.html", "<!DOCTYPE html><p>{{name}}</p>");
    dir.write("code/pages/hello.ms", "render = function(req)\n  return {\"name\": \"x\"}\nend function\n");
    dir.write("code/pages/plain.html", "<!DOCTYPE html><p>{{unfilled}}</p>");
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
    dir.write("code/pages/guestbook.html", "<!DOCTYPE html><p>gb</p>");
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
    dir.write("code/pages/guestbook/more.html", "<!DOCTYPE html><p>more</p>");
    CompileOutput out = compileApp(dir.path());
    CHECK_FALSE(out.ok());
    CHECK(hasDiagnostic(out, "URL /guestbook/more is also served by"));
}

TEST_CASE("compile: template and script errors stop the build, with their lines") {
    TempDir dir;
    dir.write("code/pages/a.html", "<!DOCTYPE html><p>\n<a href={{url}}>x</a></p>");
    dir.write("code/pages/b.ms", "render = function(req)\n  return (1 +\nend function\n");
    CompileOutput out = compileApp(dir.path());
    CHECK_FALSE(out.ok());
    CHECK(hasDiagnostic(out, "code/pages/a.html:2: error: a field isn't allowed in an unquoted attribute value"));
    CHECK(hasDiagnostic(out, "code/pages/b.ms:"));
}

TEST_CASE("compile: .ms files outside pages/ and lib/ are ignored, with a warning") {
    TempDir dir;
    dir.write("code/pages/index.html", "<!DOCTYPE html>home");
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
    dir.write("code/pages/404.html", "<!DOCTYPE html>missing");
    dir.write("code/pages/500.html", "<!DOCTYPE html>broken");
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

TEST_CASE("compile: golden route table for tests/apps/markdown-site") {
    checkGolden("markdown-site");
}

// --- Fragments, includes, and Markdown (M4) ---

TEST_CASE("compile: fragments and includes") {
    TempDir dir;
    dir.write("code/frags/header.html", "<header>{{title}} {{> parts/nav}}</header>");
    dir.write("code/frags/parts/nav.html", "<nav>{{user.name}}</nav>");
    dir.write("code/frags/intro.md", "# Hi\n\nThere.");
    dir.write("code/pages/index.html", "<!DOCTYPE html><body>{{> header}}{{> intro}}{{main}}</body>");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const CompiledTemplate* page = out.app->templates.find(RelPath::parse("pages/index.html").value());
    REQUIRE(page);
    CHECK_EQ(page->fieldNames(), std::vector<std::string>{"main", "title", "user"});
    CHECK(page->hasMarkdown());
    REQUIRE(out.app->templates.fragment("parts/nav"));
    REQUIRE(out.app->templates.fragment("intro"));
    CHECK(out.app->templates.fragment("intro")->isMarkdown());
    CHECK_FALSE(out.app->templates.fragment("nope"));
}

TEST_CASE("compile: fragment and include errors") {
    struct Case {
        std::vector<std::pair<const char*, const char*>> files;
        const char* diagnostic;
    };
    const Case cases[] = {
        {{{"code/frags/a.html", "<html>"}}, "code/frags/a.html:1: error: a fragment can't have <html>"},
        {{{"code/pages/p.html", "<!DOCTYPE html>\n{{> gone}}"}},
         "code/pages/p.html:2: error: there's no code/frags/gone.html"},
        {{{"code/frags/a.html", "{{> b}}"}, {"code/frags/b.html", "{{> a}}"}}, "error: include cycle: a → b → a"},
        {{{"code/frags/x.html", "<b>"}, {"code/frags/x.md", "x"}}, "the fragment \"x\" is also code/frags/x.html"},
        {{{"code/frags/notes.txt", "x"}}, "code/frags/notes.txt: warning: only .html and .md files are fragments"},
        {{{"code/pages/p.html", "<p>no doctype</p>"}}, "code/pages/p.html:1: error: a page must be a complete document"},
        {{{"code/frags/layout/markdown.html", "<!DOCTYPE html><p>{{title}}</p>"}},
         "code/frags/layout/markdown.html: error: the Markdown layout needs a {{content}} field"},
    };
    for (const Case& c : cases) {
        TempDir dir;
        dir.write("code/pages/index.html", "<!DOCTYPE html>x");
        for (const auto& [path, text] : c.files) dir.write(path, text);
        CompileOutput out = compileApp(dir.path());
        INFO(c.diagnostic);
        CHECK_MESSAGE(hasDiagnostic(out, c.diagnostic), allDiagnostics(out));
    }
}

TEST_CASE("compile: Markdown pages") {
    TempDir dir;
    dir.write("code/pages/index.md", "# Home\n\n[Guide](docs/guide.md#start) [gone](gone.md) ![i](i.png) ![j](j.png)");
    dir.write("code/pages/docs/guide.md", "# The Guide\n\n[up](../index.md) [self](#x) [ext](https://e.com/a.md)");
    dir.write("code/pages/docs/index.md", "x");
    dir.write("code/pages/i.png", "png");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const RouteEntry* home = route(out, "/");
    REQUIRE(home);
    CHECK_EQ(home->kind, RouteKind::Markdown);
    const RouteEntry* docs = route(out, "/docs");
    REQUIRE(docs);
    CHECK(docs->trailingSlash);

    const MarkdownPage& index = out.app->markdownPages.at(RelPath::parse("pages/index.md").value());
    CHECK_EQ(index.title, "Home");
    CHECK(contains(index.content.str(), "<a href=\"/docs/guide#start\">Guide</a>"));
    CHECK(contains(index.content.str(), "<a href=\"gone.md\">gone</a>"));   // left as written
    const MarkdownPage& guide = out.app->markdownPages.at(RelPath::parse("pages/docs/guide.md").value());
    CHECK(contains(guide.content.str(), "<a href=\"/\">up</a>"));
    CHECK(contains(guide.content.str(), "<a href=\"#x\">self</a>"));
    CHECK(contains(guide.content.str(), "<a href=\"https://e.com/a.md\">ext</a>"));
    CHECK(hasDiagnostic(out, "code/pages/index.md: warning: links to gone.md: there's no such page"));
    CHECK(hasDiagnostic(out, "code/pages/index.md: warning: shows j.png: there's no such page or file"));
    CHECK_FALSE(hasDiagnostic(out, "i.png"));
    CHECK_FALSE(out.app->hasMarkdownCss);
}

TEST_CASE("compile: Markdown page collisions, and markdown.css") {
    TempDir dir;
    dir.write("code/pages/about.md", "x");
    dir.write("code/pages/about.html", "<!DOCTYPE html>x");
    CompileOutput out = compileApp(dir.path());
    CHECK_FALSE(out.ok());
    CHECK(hasDiagnostic(out, "URL /about is also served by"));

    TempDir css;
    css.write("code/pages/index.md", "x");
    css.write("code/pages/markdown.css", ".markdown {}");
    css.write("code/pages/404.md", "x");
    CompileOutput withCss = compileApp(css.path());
    REQUIRE(withCss.ok());
    CHECK(withCss.app->hasMarkdownCss);
    CHECK(hasDiagnostic(withCss, "code/pages/404.md: warning: the 404 page must be 404.html"));
}

TEST_CASE("compile: local links in templates must lead somewhere") {
    TempDir dir;
    dir.write("code/pages/index.html",
              "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"style.css\">"
              "<script src=\"/__fourfold/htmx.js\"></script></head><body>\n"
              "<a href=\"docs/\">ok</a> <a href=\"docs\">ok</a> <a href=\"about\">ok</a> <a href=\"#top\">ok</a>\n"
              "<a href=\"https://example.com/x.html\">ok</a> <a href=\"mailto:a@b.c\">ok</a> <a href=\"?q=1\">ok</a>\n"
              "<a href=\"about.html\">old</a>\n"
              "<a href=\"docs/index.html#x\">old index</a>\n"
              "<img src=\"img/gone.png\">\n"
              "<a href=\"{{dynamic}}/x\">fields aren't checked</a> <div hx-get=\"/guest/more\"></div>\n"
              "<a href=\"x.dat\">there, but not served</a>\n"
              "<a href=\"../up\">above</a>\n"
              "{{> nav}}\n"
              "</body></html>");
    dir.write("code/pages/about.html", "<!DOCTYPE html><a href=\"about.html?a=1\">1</a><a href=\"about.html?a=2\">2</a>");
    dir.write("code/pages/docs/index.html", "<!DOCTYPE html><a href=\"../about\">ok</a><a href=\"intro\">gone</a>");
    dir.write("code/pages/style.css", "");
    dir.write("code/pages/x.dat", "");
    dir.write("code/pages/guest.ms", "get.more = function(req)\n  return \"\"\nend function\n");
    // A fragment's relative links resolve against the page that includes it;
    // on its own, only links from the root are checked.
    dir.write("code/frags/nav.html", "<nav><a href=\"about\">ok here</a> <a href=\"/nowhere\">gone</a></nav>");
    dir.write("code/frags/loose.html", "<a href=\"relative-unknown\">not checked</a> <a href=\"/also-gone\">gone</a>");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    std::string all = allDiagnostics(out);
    INFO(all);
    CHECK(hasDiagnostic(out, "code/pages/index.html:4: warning: href=\"about.html\": it works, by redirecting, "
                             "but pages are served without .html: write \"about\" for \"about.html\""));
    CHECK(hasDiagnostic(out, "code/pages/index.html:5: warning: href=\"docs/index.html#x\": it works, by "
                             "redirecting, but pages are served without .html: write \"docs/\" for \"docs/index.html\""));
    CHECK(hasDiagnostic(out, "code/pages/index.html:6: warning: src=\"img/gone.png\": there's no such page or file"));
    CHECK(hasDiagnostic(out, "code/pages/index.html:8: warning: href=\"x.dat\": the file is there, but .dat files "
                             "aren't served"));
    CHECK(hasDiagnostic(out, "code/pages/index.html:9: warning: href=\"../up\": it climbs above the site's root"));
    CHECK(hasDiagnostic(out, "code/pages/about.html:1: warning: href=\"about.html?a=1\": it works, by "
                             "redirecting, but pages are served without .html: write \"about\" for \"about.html\" (and 1 more "
                             "like it in this file)"));
    CHECK(hasDiagnostic(out, "code/pages/docs/index.html:1: warning: href=\"intro\": there's no such page or file"));
    CHECK(hasDiagnostic(out, "code/frags/nav.html:1: warning: href=\"/nowhere\""));
    CHECK(hasDiagnostic(out, "code/frags/loose.html:1: warning: href=\"/also-gone\""));
    CHECK_FALSE(contains(all, "ok"));
    CHECK_FALSE(contains(all, "relative-unknown"));
    CHECK_FALSE(contains(all, "dynamic"));
    CHECK_FALSE(contains(all, "htmx.js"));
    CHECK_FALSE(contains(all, "/guest/more"));
    // Each broken link once, though nav.html is checked on its own and in the page.
    size_t count = 0;
    for (size_t at = all.find("/nowhere"); at != std::string::npos; at = all.find("/nowhere", at + 1)) count++;
    CHECK_EQ(count, 1);
}

TEST_CASE("compile: routes whose code uses the session are marked") {
    TempDir dir;
    dir.write("code/pages/a.html", "<!DOCTYPE html><p>{{x}}</p>");
    dir.write("code/pages/a.ms", "render = function(req)\n  return {\"x\": session.hasIndex(\"n\")}\nend function\n");
    dir.write("code/pages/b.ms",
              "// session in a comment, and \"session\" in a string, don't count; nor does x.session\n"
              "get.go = function(req)\n  x = {\"session\": 1}\n  return str(x.session)\nend function\n");
    dir.write("code/pages/c.ms", "post.go = function(req)\n  flash \"hi\"\n  return null\nend function\n");
    dir.write("code/pages/d.ms",
              "import \"cart\"\nget.go = function(req)\n  return cart.count\nend function\n");
    dir.write("code/pages/e.ms", "import \"plain\"\nget.go = function(req)\n  return \"\"\nend function\n");
    dir.write("code/pages/f.html", "<!DOCTYPE html><p>{{flash}}</p>");
    dir.write("code/pages/g.html", "<!DOCTYPE html><p>{{other}}</p>");
    dir.write("code/lib/cart.ms", "import \"deeper\"\ncount = function\n  return 1\nend function\n");
    dir.write("code/lib/deeper.ms", "peek = function\n  return session\nend function\n");
    dir.write("code/lib/plain.ms", "x = 1\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    CHECK(route(out, "/a")->usesSession);
    CHECK_FALSE(route(out, "/b/go")->usesSession);
    CHECK(route(out, "/c/go")->usesSession);
    CHECK(route(out, "/d/go")->usesSession);   // through two imports
    CHECK_FALSE(route(out, "/e/go")->usesSession);
    CHECK(route(out, "/f")->usesSession);      // a {{flash}} field, no code
    CHECK_FALSE(route(out, "/g")->usesSession);
    CHECK(contains(describe(*out.app), "/a  page  pages/a.html  fields: x  render in pages/a.ms  session"));
}

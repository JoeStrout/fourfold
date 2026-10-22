#include "pipeline/engine.h"

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

RelPath rel(const char* p) { return RelPath::parse(p).value(); }

// A small app: files on disk, and an image built by hand to match (these
// tests are about the pipeline, not the compiler).
struct App {
    TempDir dir;
    std::unique_ptr<Engine> engine;

    explicit App(bool withNotFoundPage = true, EngineOptions options = {localhostNames()}) {
        dir.write("code/pages/index.html", "<html>home</html>");
        dir.write("code/pages/about.html", "<html>about</html>");
        dir.write("code/pages/docs/index.html", "<html>docs</html>");
        dir.write("code/pages/evil.com/index.html", "<html>a folder named evil.com</html>");
        dir.write("code/pages/style.css", "body{}");
        dir.write("code/pages/404.html", "<html>missing</html>");

        auto image = std::make_shared<AppImage>();
        image->routes.add("/", {RouteKind::Page, rel("pages/index.html")});
        image->routes.add("/about", {RouteKind::Page, rel("pages/about.html")});
        image->routes.add("/docs", {RouteKind::Page, rel("pages/docs/index.html"), true});
        image->routes.add("/evil.com", {RouteKind::Page, rel("pages/evil.com/index.html"), true});
        image->routes.add("/style.css", {RouteKind::Static, rel("pages/style.css")});
        image->routes.add("/gone.css", {RouteKind::Static, rel("pages/gone.css")});
        image->statics.add(rel("pages/style.css"), {"text/css; charset=utf-8", 6, 0});
        if (withNotFoundPage) image->notFoundPage = rel("pages/404.html");

        auto app = AppFolder::open(dir.path(), AppFolder::Missing::Create);
        REQUIRE(app.ok());
        auto code = app.value().codeTree();
        REQUIRE(code.ok());
        engine = std::make_unique<Engine>(image, std::move(code).value(), options);
    }

    HttpResponse get(const std::string& target, const std::string& method = "GET",
                     const char* host = "localhost:8080") {
        HttpRequest r;
        r.method = method;
        r.target = target;
        if (host) r.headers.add("Host", host);
        return engine->handle(r);
    }
};

std::string header(const HttpResponse& r, const char* name) {
    const std::string* v = r.headers.get(name);
    return v ? *v : "(none)";
}

}  // namespace

TEST_CASE("Engine: serves pages and static files") {
    App app;
    HttpResponse home = app.get("/");
    CHECK_EQ(home.status, 200);
    CHECK_EQ(home.body, "<html>home</html>");
    CHECK_EQ(header(home, "Content-Type"), "text/html; charset=utf-8");

    HttpResponse about = app.get("/about?ref=x");
    CHECK_EQ(about.status, 200);
    CHECK_EQ(about.body, "<html>about</html>");

    HttpResponse css = app.get("/style.css");
    CHECK_EQ(css.status, 200);
    CHECK_EQ(css.body, "body{}");
    CHECK_EQ(header(css, "Content-Type"), "text/css; charset=utf-8");

    CHECK_EQ(app.get("/docs/").body, "<html>docs</html>");
    CHECK_EQ(app.get("//docs/").status, 200);   // repeated slashes collapse
}

TEST_CASE("Engine: each route has one canonical URL; the other form redirects") {
    App app;
    struct Case {
        const char* target;
        const char* location;
    };
    for (const Case& c : {
             Case{"/docs", "/docs/"},                   // a folder's index page wants the slash
             Case{"/docs?x=1&y=2", "/docs/?x=1&y=2"},   // the query is kept
             Case{"/about/", "/about"},                 // other pages don't
             Case{"/about//", "/about"},
             Case{"/style.css/", "/style.css"},
             Case{"//docs", "/docs/"},
             Case{"//evil.com", "/evil.com/"},          // never "//evil.com/": that's another site
             Case{"/%64ocs", "/docs/"},
         }) {
        CAPTURE(c.target);
        HttpResponse r = app.get(c.target);
        CHECK_EQ(r.status, 302);
        CHECK_EQ(header(r, "Location"), c.location);
    }
}

TEST_CASE("Engine: not found uses pages/404.html, with status 404") {
    App app;
    HttpResponse r = app.get("/nope");
    CHECK_EQ(r.status, 404);
    CHECK_EQ(r.body, "<html>missing</html>");
    CHECK_EQ(header(r, "Content-Type"), "text/html; charset=utf-8");

    CHECK_EQ(app.get("/404").status, 404);
    CHECK_EQ(app.get("/About").status, 404);       // case matters
    CHECK_EQ(app.get("/index").status, 404);
    CHECK_EQ(app.get("/about.html").status, 404);
    CHECK_EQ(app.get("/gone.css").status, 404);    // in the image, but deleted since

    App bare(false);
    HttpResponse plain = bare.get("/nope");
    CHECK_EQ(plain.status, 404);
    CHECK_EQ(plain.body, "Not Found\n");
}

TEST_CASE("Engine: bad paths are 400") {
    App app;
    for (const char* target : {"/../etc/passwd", "/%2e%2e/x", "/a%2Fb", "/a%00b", "about", "/\xFF"}) {
        CAPTURE(target);
        CHECK_EQ(app.get(target).status, 400);
    }
}

TEST_CASE("Engine: only localhost Host names in dev") {
    App app;
    CHECK_EQ(app.get("/", "GET", "localhost").status, 200);
    CHECK_EQ(app.get("/", "GET", "LOCALHOST:8080").status, 200);
    CHECK_EQ(app.get("/", "GET", "127.0.0.1:8080").status, 200);
    CHECK_EQ(app.get("/", "GET", "[::1]:8080").status, 200);
    CHECK_EQ(app.get("/", "GET", "evil.example:8080").status, 400);   // DNS rebinding
    CHECK_EQ(app.get("/", "GET", "localhost.evil.example").status, 400);
    CHECK_EQ(app.get("/", "GET", nullptr).status, 400);

    HttpRequest twoHosts;
    twoHosts.method = "GET";
    twoHosts.target = "/";
    twoHosts.headers.add("Host", "localhost");
    twoHosts.headers.add("Host", "evil.example");
    CHECK_EQ(app.engine->handle(twoHosts).status, 400);

    App anyHost(true, EngineOptions{});
    CHECK_EQ(anyHost.get("/", "GET", "anything.example").status, 200);
}

TEST_CASE("Engine: GET and HEAD only, for now") {
    App app;
    CHECK_EQ(app.get("/about", "HEAD").status, 200);
    HttpResponse post = app.get("/about", "POST");
    CHECK_EQ(post.status, 405);
    CHECK_EQ(header(post, "Allow"), "GET, HEAD");
    CHECK_EQ(app.get("/nope", "POST").status, 404);
}

TEST_CASE("Engine: every response gets the security headers") {
    App app;
    for (const char* target : {"/", "/style.css", "/nope", "/docs", "/../x"}) {
        CAPTURE(target);
        HttpResponse r = app.get(target);
        CHECK(header(r, "Content-Security-Policy").find("frame-ancestors 'none'") != std::string::npos);
        CHECK(header(r, "Content-Security-Policy").find("script-src 'self';") != std::string::npos);
        CHECK_EQ(header(r, "X-Frame-Options"), "DENY");
        CHECK_EQ(header(r, "X-Content-Type-Options"), "nosniff");
        CHECK_EQ(header(r, "Referrer-Policy"), "strict-origin-when-cross-origin");
        CHECK_EQ(header(r, "Cross-Origin-Opener-Policy"), "same-origin");
        CHECK(header(r, "Permissions-Policy").find("camera=()") != std::string::npos);
        CHECK_EQ(header(r, "Cache-Control"), "no-cache");
    }
}

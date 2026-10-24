#include "pipeline/engine.h"

#include <future>
#include <mutex>

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

RelPath rel(const char* p) { return RelPath::parse(p).value(); }

CodeTree codeTree(const TempDir& dir) {
    auto folder = AppFolder::open(dir.path(), AppFolder::Missing::Create);
    REQUIRE(folder.ok());
    auto code = folder.value().codeTree();
    REQUIRE(code.ok());
    return std::move(code).value();
}

// Pages are served from their compiled templates; these tests build the
// compiled app by hand (they're about the pipeline, not the compiler).
void addTemplate(CompiledApp& app, const CodeTree& code, const char* path) {
    auto file = code.read(rel(path));
    REQUIRE(file.ok());
    ParsedTemplate parsed = parseTemplate(file.value());
    REQUIRE(parsed.ok());
    app.templates.add(rel(path), std::move(parsed.compiled));
}

// Sends a request and waits for the response, which may come from another thread.
HttpResponse send(const Engine& engine, const HttpRequest& request) {
    auto promise = std::make_shared<std::promise<HttpResponse>>();
    auto future = promise->get_future();
    engine.handle(request, [promise](HttpResponse r) { promise->set_value(std::move(r)); });
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return future.get();
}

HttpRequest makeRequest(const std::string& target, const std::string& method = "GET",
                        const char* host = "localhost:8080") {
    HttpRequest r;
    r.method = method;
    r.target = target;
    if (host) r.headers.add("Host", host);
    return r;
}

// A small app: files on disk, and a compiled app built by hand to match (these
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

        auto compiled = std::make_shared<CompiledApp>();
        compiled->routes.add("/", {RouteKind::Page, rel("pages/index.html")});
        compiled->routes.add("/about", {RouteKind::Page, rel("pages/about.html")});
        compiled->routes.add("/docs", {RouteKind::Page, rel("pages/docs/index.html"), true});
        compiled->routes.add("/evil.com", {RouteKind::Page, rel("pages/evil.com/index.html"), true});
        compiled->routes.add("/style.css", {RouteKind::Static, rel("pages/style.css")});
        compiled->routes.add("/gone.css", {RouteKind::Static, rel("pages/gone.css")});
        compiled->statics.add(rel("pages/style.css"), {"text/css; charset=utf-8", 6, 0});
        if (withNotFoundPage) compiled->notFoundPage = rel("pages/404.html");

        CodeTree code = codeTree(dir);
        for (const char* page : {"pages/index.html", "pages/about.html", "pages/docs/index.html",
                                 "pages/evil.com/index.html", "pages/404.html"}) {
            addTemplate(*compiled, code, page);
        }
        engine = std::make_unique<Engine>(compiled, std::move(code), nullptr, options);
    }

    HttpResponse get(const std::string& target, const std::string& method = "GET",
                     const char* host = "localhost:8080") {
        return send(*engine, makeRequest(target, method, host));
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
    CHECK_EQ(app.get("/gone.css").status, 404);    // in the compiled app, but deleted since

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
    CHECK_EQ(send(*app.engine, twoHosts).status, 400);

    App anyHost(true, EngineOptions{});
    CHECK_EQ(anyHost.get("/", "GET", "anything.example").status, 200);
}

TEST_CASE("Engine: pages answer GET and HEAD only") {
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

// --- Pages and handlers with code ---

namespace {

// A small app with code: .ms files compiled by a real ScriptHost, routes
// built by hand.
struct ScriptApp {
    TempDir dir;
    std::unique_ptr<ScriptHost> scripts;
    std::unique_ptr<Engine> engine;
    std::vector<ScriptError> errors;
    std::mutex errorsMutex;

    explicit ScriptApp(bool showErrorDetails = false) {
        dir.write("code/pages/hello.html", "<p>Hello, {{name}}!</p>");
        dir.write("code/pages/hello.ms",
                  "render = function(req)\n"
                  "  name = \"world\"\n"
                  "  if req.query.hasIndex(\"name\") then name = req.query.name\n"
                  "  return {\"name\": name}\n"
                  "end function\n"
                  "get.text = function(req)\n  return \"text for \" + req.path\nend function\n"
                  "post.form = function(req)\n  return json(req.form)\nend function\n"
                  "get.go = function(req)\n  return redirect(\"/hello?name=again\")\nend function\n"
                  "get.boom = function(req)\n  x = 1\n  return nosuch\nend function\n"
                  "get.nothing = function(req)\n  return null\nend function\n");
        dir.write("code/pages/api.ms", "render = function(req)\n  return {\"a\": 1}\nend function\n");
        dir.write("code/pages/500.html", "<p>Sorry.</p>");

        auto host = ScriptHost::create();
        REQUIRE(host.ok());
        scripts = std::move(host).value();
        CodeTree code = codeTree(dir);
        std::vector<CodeFile> pages;
        for (const char* path : {"pages/hello.ms", "pages/api.ms"}) pages.push_back(code.read(rel(path)).value());
        CompiledScripts compiled = scripts->compileScripts(pages, {});
        REQUIRE(compiled.bundle);

        auto app = std::make_shared<CompiledApp>();
        app->scripts = compiled.bundle;
        RelPath helloMs = rel("pages/hello.ms");
        RouteEntry hello{RouteKind::Page, rel("pages/hello.html")};
        hello.render = HandlerRef{HandlerRef::Kind::Render, helloMs, "render"};
        app->routes.add("/hello", hello);
        auto handler = [&](const char* url, HandlerRef::Kind kind, const char* name) {
            RouteEntry e{RouteKind::Handler, helloMs};
            (kind == HandlerRef::Kind::Get ? e.get : e.post) = HandlerRef{kind, helloMs, name};
            app->routes.add(url, e);
        };
        handler("/hello/text", HandlerRef::Kind::Get, "text");
        handler("/hello/form", HandlerRef::Kind::Post, "form");
        handler("/hello/go", HandlerRef::Kind::Get, "go");
        handler("/hello/boom", HandlerRef::Kind::Get, "boom");
        handler("/hello/nothing", HandlerRef::Kind::Get, "nothing");
        RouteEntry api{RouteKind::Page, rel("pages/api.ms")};
        api.render = HandlerRef{HandlerRef::Kind::Render, rel("pages/api.ms"), "render"};
        app->routes.add("/api", api);
        addTemplate(*app, code, "pages/hello.html");
        addTemplate(*app, code, "pages/500.html");
        app->errorPage = rel("pages/500.html");

        EngineOptions options{localhostNames()};
        options.showErrorDetails = showErrorDetails;
        options.onScriptError = [this](const ScriptError& e) {
            std::lock_guard<std::mutex> lock(errorsMutex);
            errors.push_back(e);
        };
        engine = std::make_unique<Engine>(app, std::move(code), scripts.get(), options);
    }

    HttpResponse get(const std::string& target, const std::string& method = "GET") {
        return send(*engine, makeRequest(target, method));
    }

    HttpResponse post(const std::string& target, const std::string& body, const char* type) {
        HttpRequest r = makeRequest(target, "POST");
        if (type) r.headers.add("Content-Type", type);
        r.body = body;
        return send(*engine, r);
    }
};

}  // namespace

TEST_CASE("Engine: render fills the page's template, escaping values") {
    ScriptApp app;
    HttpResponse r = app.get("/hello");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "<p>Hello, world!</p>");
    CHECK_EQ(header(r, "Content-Type"), "text/html; charset=utf-8");
    CHECK_EQ(app.get("/hello?name=%3Cb%3EAda%3C%2Fb%3E").body, "<p>Hello, &lt;b&gt;Ada&lt;/b&gt;!</p>");
    CHECK_EQ(app.get("/hello?name=Ada+L").body, "<p>Hello, Ada L!</p>");
    CHECK_EQ(app.get("/hello", "HEAD").status, 200);
    CHECK(header(r, "Content-Security-Policy") != "(none)");
}

TEST_CASE("Engine: handlers' return values become responses") {
    ScriptApp app;
    HttpResponse text = app.get("/hello/text");
    CHECK_EQ(text.status, 200);
    CHECK_EQ(text.body, "text for /hello/text");
    CHECK_EQ(header(text, "Content-Type"), "text/plain; charset=utf-8");

    HttpResponse go = app.get("/hello/go");
    CHECK_EQ(go.status, 303);
    CHECK_EQ(header(go, "Location"), "/hello?name=again");

    HttpResponse nothing = app.get("/hello/nothing");
    CHECK_EQ(nothing.status, 204);
    CHECK(nothing.body.empty());
    CHECK_EQ(header(nothing, "X-Frame-Options"), "DENY");
}

TEST_CASE("Engine: POST handlers get url-encoded form fields") {
    ScriptApp app;
    HttpResponse r = app.post("/hello/form", "who=Ada+L&msg=a%26b&who=second", "application/x-www-form-urlencoded");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(header(r, "Content-Type"), "application/json");
    CHECK_EQ(r.body, "{\"msg\":\"a\\u0026b\",\"who\":\"Ada L\"}");
    CHECK_EQ(app.post("/hello/form", "who=x", "application/x-www-form-urlencoded; charset=UTF-8").body,
             "{\"who\":\"x\"}");
    CHECK_EQ(app.post("/hello/form", "who=x", "text/plain").body, "{}");
    CHECK_EQ(app.post("/hello/form", "who=x", nullptr).body, "{}");
}

TEST_CASE("Engine: methods a route doesn't handle are 405, with Allow") {
    ScriptApp app;
    HttpResponse r = app.get("/hello/form");
    CHECK_EQ(r.status, 405);
    CHECK_EQ(header(r, "Allow"), "POST");
    r = app.post("/hello", "", nullptr);
    CHECK_EQ(r.status, 405);
    CHECK_EQ(header(r, "Allow"), "GET, HEAD");
    r = app.post("/hello/text", "", nullptr);
    CHECK_EQ(r.status, 405);
    CHECK_EQ(header(r, "Allow"), "GET, HEAD");
    CHECK_EQ(app.get("/hello/text", "DELETE").status, 405);
}

TEST_CASE("Engine: a handler's URL has one canonical form; a POST to the other isn't redirected") {
    ScriptApp app;
    HttpResponse r = app.get("/hello/text/");
    CHECK_EQ(r.status, 302);
    CHECK_EQ(header(r, "Location"), "/hello/text");
    CHECK_EQ(app.post("/hello/form/", "", nullptr).status, 404);
}

TEST_CASE("Engine: script errors are 500s, with the 500 page or (in dev) the details") {
    SUBCASE("production: the 500 page, details only to the log") {
        ScriptApp app(false);
        HttpResponse r = app.get("/hello/boom");
        CHECK_EQ(r.status, 500);
        CHECK_EQ(r.body, "<p>Sorry.</p>");
        CHECK(r.body.find("nosuch") == std::string::npos);
        std::lock_guard<std::mutex> lock(app.errorsMutex);
        REQUIRE_EQ(app.errors.size(), 1u);
        CHECK_EQ(app.errors[0].file, "code/pages/hello.ms");
        CHECK_EQ(app.errors[0].line, 17);
    }
    SUBCASE("dev: plain-text details") {
        ScriptApp app(true);
        HttpResponse r = app.get("/hello/boom");
        CHECK_EQ(r.status, 500);
        CHECK_EQ(header(r, "Content-Type"), "text/plain; charset=utf-8");
        CHECK(r.body.find("code/pages/hello.ms line 17") != std::string::npos);
        CHECK(r.body.find("nosuch") != std::string::npos);
    }
}

TEST_CASE("Engine: render returning fields for a page with no template is an error") {
    ScriptApp app(true);
    HttpResponse r = app.get("/api");
    CHECK_EQ(r.status, 500);
    CHECK(r.body.find("no .html template") != std::string::npos);
}

TEST_CASE("parseUrlEncoded") {
    using Pairs = RequestFacts::Pairs;
    CHECK_EQ(parseUrlEncoded(""), Pairs{});
    CHECK_EQ(parseUrlEncoded("a=1&b=two+words"), Pairs{{"a", "1"}, {"b", "two words"}});
    CHECK_EQ(parseUrlEncoded("a&=x&&b="), Pairs{{"a", ""}, {"", "x"}, {"b", ""}});
    CHECK_EQ(parseUrlEncoded("q=caf%C3%A9%21"), Pairs{{"q", "caf\xC3\xA9!"}});
    CHECK_EQ(parseUrlEncoded("bad=%zz%4"), Pairs{{"bad", "%zz%4"}});
    CHECK_EQ(parseUrlEncoded("x=%FF%41"), Pairs{{"x", "\xEF\xBF\xBD" "A"}});   // invalid UTF-8 → U+FFFD
    CHECK_EQ(parseUrlEncoded("eq=a=b"), Pairs{{"eq", "a=b"}});
}

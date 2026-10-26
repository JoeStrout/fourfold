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
    // Whole documents are pages; the rest are parsed as fragments, which is
    // all these tests need (the compiler checks pages' shape).
    const std::string& text = file.value().contents();
    bool page = text.compare(0, 5, "<html") == 0 || text.compare(0, 9, "<!DOCTYPE") == 0;
    ParsedTemplate parsed = parseTemplate(file.value(), page ? TemplateKind::Page : TemplateKind::Fragment);
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

// The engine key these tests give their engines, so they can make CSRF
// tokens the engine accepts.
const std::string kTestKey(32, 'k');

// Adds a CSRF cookie and its token, as a page from the engine would give a
// browser.
void addCsrf(HttpRequest& r) {
    std::string cookie = PolicyGate::newCsrfCookie();
    r.headers.add("Cookie", "ff_csrf=" + cookie);
    r.headers.add("X-CSRF-Token", PolicyGate(kTestKey).tokenFor(cookie));
}

HttpRequest makeRequest(const std::string& target, const std::string& method = "GET",
                        const char* host = "localhost:8080") {
    HttpRequest r;
    r.method = method;
    r.target = target;
    if (host) r.headers.add("Host", host);
    if (method == "POST") addCsrf(r);
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

TEST_CASE("Engine: a page asked for by its file name redirects to its URL") {
    App app;
    struct Case {
        const char* target;
        const char* location;
    };
    for (const Case& c : {Case{"/about.html", "/about"}, Case{"/about.HTML?x=1", "/about?x=1"},
                          Case{"/index.html", "/"}, Case{"/docs/index.html", "/docs/"},
                          Case{"/evil.com/index.html", "/evil.com/"}}) {
        CAPTURE(c.target);
        HttpResponse r = app.get(c.target);
        CHECK_EQ(r.status, 302);
        CHECK_EQ(header(r, "Location"), c.location);
    }
    // Only for pages that exist, only for GET, and not for other routes.
    CHECK_EQ(app.get("/nope.html").status, 404);
    CHECK_EQ(app.get("/about.html/").status, 404);
    CHECK_EQ(app.get("/about.html", "POST").status, 404);
    CHECK_EQ(app.get("/style.css.html").status, 404);
    CHECK_EQ(app.get("/docs/.html").status, 404);
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
    CHECK_EQ(app.get("/about.htm").status, 404);   // (/about.html redirects; see below)
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
        CHECK(header(r, "Content-Security-Policy").find("script-src 'self' 'nonce-") != std::string::npos);
        CHECK_EQ(header(r, "X-Frame-Options"), "DENY");
        CHECK_EQ(header(r, "X-Content-Type-Options"), "nosniff");
        CHECK_EQ(header(r, "Referrer-Policy"), "strict-origin-when-cross-origin");
        CHECK_EQ(header(r, "Cross-Origin-Opener-Policy"), "same-origin");
        CHECK(header(r, "Permissions-Policy").find("camera=()") != std::string::npos);
        CHECK_EQ(header(r, "Cache-Control"), "no-cache");
    }
}

// --- Swapping apps, and the dev tools' hooks ---

namespace {

struct FakeHooks : EngineHooks {
    std::vector<std::string> reservedPaths;
    std::vector<uint64_t> decorated;   // generations
    std::mutex mutex;

    std::optional<HttpResponse> reserved(const HttpRequest&, const AdmittedRequest& admitted) override {
        std::lock_guard<std::mutex> lock(mutex);
        reservedPaths.push_back(admitted.path.path);
        if (admitted.path.path != "/__dev/ping") return std::nullopt;
        return HttpResponse::text(200, "pong");
    }
    void decorate(HttpResponse& response, uint64_t generation, const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex);
        decorated.push_back(generation);
        response.body += "<!--dev-->";
    }
    HttpResponse scriptErrorPage(const ScriptError& error) override {
        HttpResponse r = HttpResponse::text(500, "overlay: " + error.message);
        r.headers.set("Content-Type", "text/html; charset=utf-8");
        return r;
    }
};

std::shared_ptr<CompiledApp> oneStaticApp(uint64_t generation, const char* css) {
    auto app = std::make_shared<CompiledApp>();
    app->generation = generation;
    app->routes.add(std::string("/") + css, {RouteKind::Static, rel((std::string("pages/") + css).c_str())});
    app->statics.add(rel((std::string("pages/") + css).c_str()), {"text/css; charset=utf-8", 6, 0});
    return app;
}

}  // namespace

// --- Pages and handlers with code ---

namespace {

// A small app with code: .ms files compiled by a real ScriptHost, routes
// built by hand.
struct ScriptApp : Observer {
    TempDir dir;
    std::unique_ptr<ScriptHost> scripts;
    std::unique_ptr<Engine> engine;
    std::vector<ScriptError> errors;
    std::mutex errorsMutex;

    void scriptError(const ScriptError& e) override {
        std::lock_guard<std::mutex> lock(errorsMutex);
        errors.push_back(e);
    }

    FakeHooks hooks;

    explicit ScriptApp(bool showErrorDetails = false, bool withHooks = false) {
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
        options.engineKey = kTestKey;
        options.showErrorDetails = showErrorDetails;
        options.observer = this;
        if (withHooks) options.hooks = &hooks;
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
    SUBCASE("dev, with the dev tools: their error page") {
        ScriptApp app(true, true);
        HttpResponse r = app.get("/hello/boom");
        CHECK_EQ(r.status, 500);
        CHECK(r.body.find("overlay: ") == 0);
        CHECK(r.body.find("<!--dev-->") != std::string::npos);
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
    CHECK(r.body.find("there's no code/pages/api.html template") != std::string::npos);
}

TEST_CASE("Engine: setApp swaps the app for later requests") {
    App app;
    CHECK_EQ(app.get("/about").status, 200);
    std::weak_ptr<const CompiledApp> first = app.engine->app();
    app.engine->setApp(oneStaticApp(2, "style.css"));
    CHECK_EQ(app.engine->app()->generation, 2u);
    CHECK_EQ(app.get("/about").status, 404);
    CHECK_EQ(app.get("/style.css").status, 200);
    CHECK(first.expired());   // nothing else held it
}

TEST_CASE("Engine: with no app yet, pages are 503") {
    App app;
    app.engine->setApp(nullptr);
    HttpResponse r = app.get("/about");
    CHECK_EQ(r.status, 503);
    CHECK_EQ(header(r, "Content-Type"), "text/html; charset=utf-8");
    CHECK(r.body.find("hasn't compiled") != std::string::npos);
    CHECK_EQ(header(r, "X-Content-Type-Options"), "nosniff");
}

TEST_CASE("Engine: /__dev/ goes to the hooks, after the Host check") {
    FakeHooks hooks;
    EngineOptions options{localhostNames()};
    options.hooks = &hooks;
    App app(true, options);

    HttpResponse r = app.get("/__dev/ping");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "pong");
    CHECK_EQ(header(r, "X-Frame-Options"), "DENY");
    CHECK_EQ(app.get("/__dev/other").status, 404);
    CHECK_EQ(app.get("/__dev").status, 404);
    CHECK_EQ(app.get("/__dev/ping", "GET", "evil.example").status, 400);   // DNS rebinding
    std::lock_guard<std::mutex> lock(hooks.mutex);
    CHECK_EQ(hooks.reservedPaths, std::vector<std::string>{"/__dev/ping", "/__dev/other", "/__dev"});
}

TEST_CASE("Engine: without hooks, /__dev/ is not found") {
    App app;
    CHECK_EQ(app.get("/__dev/ping").status, 404);
}

TEST_CASE("Engine: hooks decorate HTML responses only, with their generation") {
    FakeHooks hooks;
    EngineOptions options{localhostNames()};
    options.hooks = &hooks;
    App app(true, options);
    auto withGeneration = std::make_shared<CompiledApp>(*app.engine->app());
    withGeneration->generation = 7;
    app.engine->setApp(withGeneration);

    CHECK_EQ(app.get("/about").body, "<html>about</html><!--dev-->");
    CHECK_EQ(app.get("/nope").body, "<html>missing</html><!--dev-->");   // the 404 page
    CHECK_EQ(app.get("/style.css").body, "body{}");
    app.engine->setApp(nullptr);
    CHECK(app.get("/about").body.find("<!--dev-->") != std::string::npos);
    std::lock_guard<std::mutex> lock(hooks.mutex);
    CHECK_EQ(hooks.decorated, std::vector<uint64_t>{7, 7, 0});
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

// --- M4: nonces, the engine's own files, fragments, HTMX, Markdown ---

namespace {

// The nonce in a response's CSP.
std::string cspNonce(const HttpResponse& r) {
    std::string csp = header(r, "Content-Security-Policy");
    size_t at = csp.find("'nonce-");
    if (at == std::string::npos) return "";
    size_t end = csp.find('\'', at + 7);
    return csp.substr(at + 7, end - at - 7);
}

// An app with fragments, a page with scripts, a Markdown page, and handlers
// that return fragments and redirects.
struct HtmlApp {
    TempDir dir;
    std::unique_ptr<ScriptHost> scripts;
    std::unique_ptr<Engine> engine;
    FakeHooks hooks;
    std::shared_ptr<CompiledApp> app;

    explicit HtmlApp(bool withLayout = false) {
        dir.write("code/pages/book.html",
                  "<!DOCTYPE html><html><head><script src=\"/__fourfold/htmx.js\"></script></head>"
                  "<body><ul>{{entries}}</ul><script>start({{config}})</script></body></html>");
        dir.write("code/pages/book.ms",
                  "render = function(req)\n"
                  "  return {\"entries\": frag(\"entry\", [{\"who\": \"<a>\"}, {\"who\": \"b\"}]), "
                  "\"config\": {\"n\": 1}}\n"
                  "end function\n"
                  "post.sign = function(req)\n"
                  "  if req.htmx then return frag(\"entry\", {\"who\": req.form.who})\n"
                  "  return redirect(\"/book\")\n"
                  "end function\n"
                  "get.scripted = function(req)\n  return frag(\"scripted\")\nend function\n"
                  "get.status = function(req)\n  return status(422, frag(\"entry\", {\"who\": \"x\"}))\nend function\n"
                  "get.md = function(req)\n  return markdown(\"# Hi <b>\")\nend function\n");
        dir.write("code/frags/entry.html", "<li>{{who}}</li>");
        dir.write("code/frags/scripted.html", "<script>evil()</script>");
        dir.write("code/frags/layout/markdown.html",
                  "<!DOCTYPE html><html><head><title>{{title}}</title></head><body>{{content}}</body></html>");

        auto host = ScriptHost::create();
        REQUIRE(host.ok());
        scripts = std::move(host).value();
        CodeTree code = codeTree(dir);
        CompiledScripts compiled = scripts->compileScripts({code.read(rel("pages/book.ms")).value()}, {});
        REQUIRE(compiled.bundle);

        app = std::make_shared<CompiledApp>();
        app->generation = 3;
        app->scripts = compiled.bundle;
        RelPath ms = rel("pages/book.ms");
        RouteEntry book{RouteKind::Page, rel("pages/book.html")};
        book.render = HandlerRef{HandlerRef::Kind::Render, ms, "render"};
        app->routes.add("/book", book);
        auto handler = [&](const char* url, HandlerRef::Kind kind, const char* name) {
            RouteEntry e{RouteKind::Handler, ms};
            (kind == HandlerRef::Kind::Get ? e.get : e.post) = HandlerRef{kind, ms, name};
            app->routes.add(url, e);
        };
        handler("/book/sign", HandlerRef::Kind::Post, "sign");
        handler("/book/scripted", HandlerRef::Kind::Get, "scripted");
        handler("/book/status", HandlerRef::Kind::Get, "status");
        handler("/book/md", HandlerRef::Kind::Get, "md");
        addTemplate(*app, code, "pages/book.html");
        FragmentMap frags;
        for (const char* name : {"entry", "scripted"}) {
            std::string path = std::string("frags/") + name + ".html";
            frags[name] = parseTemplate(code.read(rel(path.c_str())).value(), TemplateKind::Fragment).compiled;
        }
        app->templates.setFragments(std::move(frags));

        // A Markdown page, rendered by hand as the compiler would.
        app->routes.add("/guide", {RouteKind::Markdown, rel("pages/guide.md")});
        app->markdownPages[rel("pages/guide.md")] =
            MarkdownPage{HtmlAccess::make("<div class=\"markdown\"><h1 id=\"guide\">Guide</h1></div>", false, true),
                         "Guide & more"};
        if (withLayout) {
            app->markdownLayout =
                parseTemplate(code.read(rel("frags/layout/markdown.html")).value(), TemplateKind::Page).compiled;
        }

        EngineOptions options{localhostNames()};
        options.engineKey = kTestKey;
        options.hooks = &hooks;
        engine = std::make_unique<Engine>(app, std::move(code), scripts.get(), options);
    }

    HttpResponse get(const std::string& target) { return send(*engine, makeRequest(target)); }

    HttpResponse post(const std::string& target, const std::string& body, bool htmx) {
        HttpRequest r = makeRequest(target, "POST");
        r.headers.add("Content-Type", "application/x-www-form-urlencoded");
        if (htmx) r.headers.add("HX-Request", "true");
        r.body = body;
        return send(*engine, r);
    }
};

}  // namespace

TEST_CASE("Engine: each response has a fresh nonce, on every script it renders") {
    HtmlApp app;
    HttpResponse a = app.get("/book");
    HttpResponse b = app.get("/book");
    REQUIRE_EQ(a.status, 200);
    std::string nonce = cspNonce(a);
    CHECK_EQ(nonce.size(), 24);   // 16 bytes, base64
    CHECK_NE(nonce, cspNonce(b));
    // Every <script> in the page carries it: the page's two, and the dev tools'.
    size_t scripts = 0, nonced = 0;
    for (size_t at = a.body.find("<script"); at != std::string::npos; at = a.body.find("<script", at + 1)) {
        scripts++;
        if (a.body.compare(at, 15 + nonce.size(), "<script nonce=\"" + nonce) == 0) nonced++;
    }
    CHECK_EQ(scripts, 2);
    CHECK_EQ(nonced, 2);
    CHECK(a.body.find("<li>&lt;a&gt;</li><li>b</li>") != std::string::npos);
    CHECK(a.body.find("start({\"n\":1})") != std::string::npos);
    std::lock_guard<std::mutex> lock(app.hooks.mutex);
    CHECK_EQ(app.hooks.decorated, std::vector<uint64_t>{3, 3});
}

TEST_CASE("Engine: /__fourfold/ serves the engine's own files") {
    HtmlApp app;
    HttpResponse htmx = app.get("/__fourfold/htmx.js");
    CHECK_EQ(htmx.status, 200);
    CHECK_EQ(header(htmx, "Content-Type"), "text/javascript; charset=utf-8");
    CHECK(htmx.body.find("var htmx=function()") == 0);
    CHECK(htmx.body.find("htmx.config.allowEval = false;") != std::string::npos);
    CHECK(htmx.body.find("htmx.config.allowScriptTags = false;") != std::string::npos);
    CHECK_EQ(header(htmx, "Cache-Control"), "public, max-age=86400");
    HttpResponse css = app.get("/__fourfold/markdown.css");
    CHECK_EQ(css.status, 200);
    CHECK(css.body.find(".markdown") != std::string::npos);
    CHECK_EQ(app.get("/__fourfold/nothing").status, 404);
    CHECK_EQ(app.get("/__fourfold").status, 404);
    CHECK_EQ(send(*app.engine, makeRequest("/__fourfold/htmx.js", "POST")).status, 404);
}

TEST_CASE("Engine: fragments, with and without HTMX") {
    HtmlApp app;
    HttpResponse frag = app.post("/book/sign", "who=%3Cscript%3E", true);
    CHECK_EQ(frag.status, 200);
    CHECK_EQ(header(frag, "Content-Type"), "text/html; charset=utf-8");
    CHECK_EQ(frag.body, "<li>&lt;script&gt;</li>");   // not decorated: it goes into a page

    HttpResponse plain = app.post("/book/sign", "who=x", false);
    CHECK_EQ(plain.status, 303);
    CHECK_EQ(header(plain, "Location"), "/book");

    HttpResponse status = app.get("/book/status");
    CHECK_EQ(status.status, 422);
    CHECK_EQ(status.body, "<li>x</li>");
    CHECK_EQ(header(status, "Content-Type"), "text/html; charset=utf-8");

    HttpResponse md = app.get("/book/md");
    CHECK_EQ(md.body, "<div class=\"markdown comment\">\n<p>Hi &lt;b&gt;</p>\n</div>\n");
}

TEST_CASE("Engine: an HTMX redirect") {
    TempDir dir;
    dir.write("code/pages/x.ms", "get.go = function(req)\n  return redirect(\"/there\")\nend function\n");
    auto host = ScriptHost::create();
    REQUIRE(host.ok());
    CodeTree code = codeTree(dir);
    CompiledScripts compiled = host.value()->compileScripts({code.read(rel("pages/x.ms")).value()}, {});
    REQUIRE(compiled.bundle);
    auto app = std::make_shared<CompiledApp>();
    app->scripts = compiled.bundle;
    RouteEntry e{RouteKind::Handler, rel("pages/x.ms")};
    e.get = HandlerRef{HandlerRef::Kind::Get, rel("pages/x.ms"), "go"};
    app->routes.add("/x/go", e);
    Engine engine(app, std::move(code), host.value().get(), EngineOptions{localhostNames()});
    HttpRequest r = makeRequest("/x/go");
    r.headers.add("HX-Request", "true");
    HttpResponse h = send(engine, r);
    CHECK_EQ(h.status, 200);
    CHECK_EQ(header(h, "HX-Redirect"), "/there");
    CHECK_FALSE(h.headers.has("Location"));
    CHECK_EQ(send(engine, makeRequest("/x/go")).status, 303);
}

TEST_CASE("Engine: a fragment response with a <script> is an error") {
    HtmlApp app;
    HttpResponse r = app.get("/book/scripted");
    CHECK_EQ(r.status, 500);
    CHECK(r.body.find("evil()") == std::string::npos);
}

TEST_CASE("Engine: Markdown pages, in the built-in page or the app's layout") {
    {
        HtmlApp builtin;   // one at a time: only one ScriptHost may exist
        HttpResponse a = builtin.get("/guide");
        CHECK_EQ(a.status, 200);
        CHECK(a.body.find("<title>Guide &amp; more</title>") != std::string::npos);
        CHECK(a.body.find("<link rel=\"stylesheet\" href=\"/__fourfold/markdown.css\">") != std::string::npos);
        CHECK(a.body.find("<h1 id=\"guide\">Guide</h1>") != std::string::npos);
    }

    HtmlApp layout(true);
    HttpResponse b = layout.get("/guide");
    CHECK_EQ(b.status, 200);
    CHECK(b.body.find("<title>Guide &amp; more</title>") != std::string::npos);
    CHECK(b.body.find("<body hx-headers=\"") != std::string::npos);   // the CSRF token, for HTMX
    CHECK(b.body.find("\"><div class=\"markdown\"><h1") != std::string::npos);
    CHECK(b.body.find("href=\"/__fourfold/markdown.css\"></head>") == std::string::npos);
    CHECK(b.body.find("<link rel=\"stylesheet\" href=\"/__fourfold/markdown.css\">\n</head>") != std::string::npos);

    // An app with its own pages/markdown.css links that one instead.
    auto own = std::make_shared<CompiledApp>(*layout.app);
    own->hasMarkdownCss = true;
    layout.engine->setApp(own);
    CHECK(layout.get("/guide").body.find("href=\"/markdown.css\"") != std::string::npos);
}

TEST_CASE("requestFacts: the engine's cookies and the CSRF token never reach req") {
    HttpRequest r = makeRequest("/x?q=1", "GET");
    r.method = "POST";
    r.headers.add("Cookie", "a=1; ff_session=SECRET; __Host-ff_csrf=C; b=2");
    r.headers.add("Cookie", "ff_csrf=D");
    r.headers.add("X-CSRF-Token", "T1");
    r.headers.add("Content-Type", "application/x-www-form-urlencoded");
    r.body = "who=Ada&_csrf=T2&_csrf=T3";
    EngineOptions options{localhostNames()};
    AdmittedRequest admitted;
    REQUIRE_FALSE(admit(r, options, admitted));
    std::optional<std::string> token;
    RequestFacts facts = requestFacts(r, admitted, &token);
    CHECK_EQ(token.value_or(""), "T1");   // the header first
    for (const auto& [name, value] : facts.headers) {
        CHECK_NE(name, "x-csrf-token");
        if (name == "cookie") CHECK_EQ(value, "a=1; b=2");
    }
    REQUIRE_EQ(facts.form.size(), 1u);
    CHECK_EQ(facts.form[0].first, "who");

    // Without the header, the form's field.
    HttpRequest f = makeRequest("/x", "GET");
    f.method = "POST";
    f.headers.add("Cookie", "ff_session=SECRET");
    f.headers.add("Content-Type", "application/x-www-form-urlencoded");
    f.body = "_csrf=T2";
    REQUIRE_FALSE(admit(f, options, admitted));
    token.reset();
    facts = requestFacts(f, admitted, &token);
    CHECK_EQ(token.value_or(""), "T2");
    for (const auto& [name, value] : facts.headers) CHECK_NE(name, "cookie");   // nothing left of it
    CHECK(facts.form.empty());
}

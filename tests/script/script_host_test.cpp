#include "script/script_host.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include "doctest.h"
#include "test_gate.h"
#include "partition/app_folder.h"
#include "template/template.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;
using SteadyClock = std::chrono::steady_clock;

namespace {

// An app folder on disk, its .ms files compiled by a ScriptHost.
struct Scripts {
    TempDir dir;
    std::unique_ptr<ScriptHost> host;
    CompiledScripts compiled;
    std::shared_ptr<CompiledApp> app = std::make_shared<CompiledApp>();
    std::vector<std::string> printed;
    std::mutex printedMutex;

    explicit Scripts(ScriptHostOptions options = testOptions()) {
        options.printOutput = [this](const std::string& file, const std::string& text) {
            std::lock_guard<std::mutex> lock(printedMutex);
            printed.push_back(file + ": " + text);
        };
        auto h = ScriptHost::create(std::move(options));
        REQUIRE(h.ok());
        host = std::move(h).value();
    }

    static ScriptHostOptions testOptions() {
        ScriptHostOptions o;
        o.testIntrinsics = true;
        o.budget.instructions = 2'000'000;
        o.budget.wallClock = std::chrono::milliseconds(3000);
        return o;
    }

    void write(const std::string& path, const std::string& text) { dir.write("code/" + path, text); }

    // Compiles every .ms file written so far, and parses the templates
    // (pages/*.html and frags/*.html) into the app, as the compiler would.
    void compile() {
        std::vector<CodeFile> pages, libs;
        readFiles(pages, libs);
        compiled = host->compileScripts(pages, libs);
        app->scripts = compiled.bundle;
        parseTemplates();
    }

    void parseTemplates() {
        auto folder = AppFolder::open(dir.path(), AppFolder::Missing::Create);
        REQUIRE(folder.ok());
        auto code = folder.value().codeTree();
        REQUIRE(code.ok());
        auto listing = code.value().list();
        REQUIRE(listing.ok());
        FragmentMap frags;
        std::vector<std::pair<RelPath, CompiledTemplate>> pageTemplates;
        for (const CodeEntry& e : listing.value().files) {
            if (e.path.extension() != ".html") continue;
            bool isFrag = e.path.str().compare(0, 6, "frags/") == 0;
            ParsedTemplate p = parseTemplate(code.value().read(e.path).value(),
                                             isFrag ? TemplateKind::Fragment : TemplateKind::Page);
            REQUIRE_MESSAGE(p.ok(), (e.path.str() + ": " + (p.ok() ? "" : p.errors[0].message)));
            if (isFrag) {
                std::string name = e.path.str().substr(6);
                frags[name.substr(0, name.size() - 5)] = std::move(p.compiled);
            } else {
                pageTemplates.emplace_back(e.path, std::move(p.compiled));
            }
        }
        REQUIRE(resolveIncludes(frags).empty());
        app->templates = TemplateSet();
        for (auto& [path, t] : pageTemplates) {
            REQUIRE(resolveIncludes(t, frags).empty());
            app->templates.add(path, std::move(t));
        }
        app->templates.setFragments(std::move(frags));
    }

    // Every .ms file written so far: pages, and libs in code/lib/.
    void readFiles(std::vector<CodeFile>& pages, std::vector<CodeFile>& libs) {
        auto folder = AppFolder::open(dir.path(), AppFolder::Missing::Create);
        REQUIRE(folder.ok());
        auto code = folder.value().codeTree();
        REQUIRE(code.ok());
        auto listing = code.value().list();
        REQUIRE(listing.ok());
        RelPath lib = RelPath::parse("lib").value();
        for (const CodeEntry& e : listing.value().files) {
            if (e.path.extension() != ".ms") continue;
            auto file = code.value().read(e.path);
            REQUIRE(file.ok());
            (e.path.parent() == lib ? libs : pages).push_back(file.value());
        }
    }

    std::string diagnostics() const {
        std::string s;
        for (const Diagnostic& d : compiled.diagnostics) s += d.toString() + "\n";
        return s;
    }

    static HandlerRef handler(const std::string& source, const std::string& label) {
        HandlerRef h;
        h.source = RelPath::parse(source).value();
        if (label == "render") {
            h.kind = HandlerRef::Kind::Render;
            h.name = "render";
        } else {
            h.kind = label.compare(0, 4, "get.") == 0 ? HandlerRef::Kind::Get : HandlerRef::Kind::Post;
            h.name = label.substr(label.find('.') + 1);
        }
        return h;
    }

    // Starts a run; the future yields its outcome.
    std::future<HandlerOutcome> start(const std::string& source, const std::string& label,
                                      RequestFacts request = {}, RenderOptions renderOptions = {}) {
        auto promise = std::make_shared<std::promise<HandlerOutcome>>();
        auto future = promise->get_future();
        RunContext context;
        context.request = std::move(request);
        context.render = std::move(renderOptions);
        host->run(app, handler(source, label), TestGate::approve(), std::move(context),
                  [promise](HandlerOutcome o) { promise->set_value(std::move(o)); });
        return future;
    }

    HandlerOutcome call(const std::string& source, const std::string& label, RequestFacts request = {},
                        RenderOptions renderOptions = {}) {
        auto future = start(source, label, std::move(request), std::move(renderOptions));
        REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        return future.get();
    }

    // Compiles a page with this source (and this template), and runs its
    // render.
    HandlerOutcome render(const std::string& source, RequestFacts request = {},
                          const std::string& html = "<!DOCTYPE html><p></p>") {
        write("pages/t.ms", source);
        write("pages/t.html", html);
        compile();
        REQUIRE_MESSAGE(compiled.bundle, diagnostics());
        return call("pages/t.ms", "render", std::move(request));
    }
};

const char* kindName(HandlerOutcome::Kind k) {
    switch (k) {
        case HandlerOutcome::Kind::Render: return "Render";
        case HandlerOutcome::Kind::Fragment: return "Fragment";
        case HandlerOutcome::Kind::Text: return "Text";
        case HandlerOutcome::Kind::Redirect: return "Redirect";
        case HandlerOutcome::Kind::Json: return "Json";
        case HandlerOutcome::Kind::Status: return "Status";
        case HandlerOutcome::Kind::NoContent: return "NoContent";
        case HandlerOutcome::Kind::Error: return "Error";
    }
    return "?";
}

// "Error: message (file:line)", for readable failures.
std::string describe(const HandlerOutcome& o) {
    std::string s = kindName(o.kind);
    if (o.kind == HandlerOutcome::Kind::Error) {
        s += ": " + o.error.message + " (" + o.error.file + ":" + std::to_string(o.error.line) + ")";
    }
    return s;
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("ScriptHost: only one may exist at a time") {
    Scripts s;
    auto second = ScriptHost::create();
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().code == ErrorCode::Conflict);
}

TEST_CASE("ScriptHost: compile errors name the file and line") {
    Scripts s;
    s.write("pages/ok.ms", "render = function(req)\n  return {}\nend function\n");
    s.write("pages/bad.ms", "x = 1\n\ny = (x +\n");
    s.compile();
    CHECK_FALSE(s.compiled.bundle);
    REQUIRE_EQ(s.compiled.diagnostics.size(), 1u);
    const Diagnostic& d = s.compiled.diagnostics[0];
    CHECK(d.severity == Diagnostic::Severity::Error);
    CHECK_EQ(d.file, "code/pages/bad.ms");
    CHECK(d.line >= 3);
}

TEST_CASE("ScriptHost: handlers are found by running the top level") {
    Scripts s;
    s.write("pages/g.ms",
            "render = function(req)\n  return {}\nend function\n"
            "get.zeta = function(req)\n  return \"z\"\nend function\n"
            "get.alpha = function\n  return \"a\"\nend function\n"
            "post.sign = function(req)\n  return null\nend function\n"
            "helper = function(x)\n  return x\nend function\n");
    s.write("pages/none.ms", "x = 42\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    const PageHandlers& g = s.compiled.pages.at(RelPath::parse("pages/g.ms").value());
    CHECK(g.render);
    CHECK_EQ(g.get, std::vector<std::string>{"alpha", "zeta"});
    CHECK_EQ(g.post, std::vector<std::string>{"sign"});
    const PageHandlers& none = s.compiled.pages.at(RelPath::parse("pages/none.ms").value());
    CHECK_FALSE(none.render);
    CHECK(none.get.empty());

    // A handler declared without a parameter is called without one.
    HandlerOutcome a = s.call("pages/g.ms", "get.alpha");
    REQUIRE_MESSAGE(a.kind == HandlerOutcome::Kind::Text, describe(a));
    CHECK_EQ(a.body, "a");
}

TEST_CASE("ScriptHost: problems found at the top level are compile errors") {
    SUBCASE("a runtime error") {
        Scripts s;
        s.write("pages/t.ms", "x = 1\ny = undefinedThing + 1\n");
        s.compile();
        CHECK_FALSE(s.compiled.bundle);
        REQUIRE_FALSE(s.compiled.diagnostics.empty());
        CHECK_EQ(s.compiled.diagnostics[0].line, 2);
    }
    SUBCASE("render that isn't a function") {
        Scripts s;
        s.write("pages/t.ms", "render = 42\n");
        s.compile();
        CHECK(contains(s.diagnostics(), "render must be a function"));
    }
    SUBCASE("get replaced, or holding a non-function") {
        Scripts s;
        s.write("pages/a.ms", "get = 3\n");
        s.write("pages/b.ms", "post.x = 3\n");
        s.compile();
        CHECK(contains(s.diagnostics(), "get must stay a map"));
        CHECK(contains(s.diagnostics(), "post.x must be a function"));
    }
    SUBCASE("an endless top level") {
        Scripts s;
        s.write("pages/t.ms", "while true\nend while\n");
        s.compile();
        CHECK(contains(s.diagnostics(), "budget"));
    }
    SUBCASE("waiting at the top level") {
        Scripts s;
        s.write("pages/t.ms", "wait 0.01\n");
        s.compile();
        CHECK(contains(s.diagnostics(), "top level"));
    }
}

TEST_CASE("ScriptHost: render fills fields from a map") {
    Scripts s;
    HandlerOutcome o = s.render(
        "render = function(req)\n"
        "  return {\"name\": \"<Ada>\", \"n\": 3, \"half\": 0.5, \"none\": null}\n"
        "end function\n",
        {}, "<!DOCTYPE html><p>{{name}}|{{n}}|{{half}}|{{none}}</p>");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Render, describe(o));
    CHECK_EQ(o.body, "<!DOCTYPE html><p>&lt;Ada&gt;|3|0.5|</p>");
    CHECK(o.warnings.empty());
}

TEST_CASE("ScriptHost: render fields must be strings, numbers, or null") {
    Scripts s;
    HandlerOutcome o = s.render("render = function(req)\n  return {\"xs\": [1, 2]}\nend function\n", {},
                                "<!DOCTYPE html>\n<p>{{xs}}</p>");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "{{xs}} is a list"));
    CHECK_EQ(o.error.file, "code/pages/t.html");
    CHECK_EQ(o.error.line, 2);
}

TEST_CASE("ScriptHost: each kind of return value") {
    Scripts s;
    s.write("pages/r.ms",
            "get.text = function(req)\n  return \"hello\"\nend function\n"
            "get.none = function(req)\n  return null\nend function\n"
            "get.go = function(req)\n  return redirect(\"/thanks?x=1\")\nend function\n"
            "get.data = function(req)\n  return json({\"a\": [1, \"two\", null], \"b\": \"</script>\"})\nend function\n"
            "get.gone = function(req)\n  return status(410, \"gone\")\nend function\n"
            "get.teapot = function(req)\n  return status(418, json([1]))\nend function\n"
            "get.empty = function(req)\n  return status(404)\nend function\n"
            "get.oops = function(req)\n  return err(\"no such entry\")\nend function\n"
            "get.map = function(req)\n  return {\"a\": 1}\nend function\n"
            "get.num = function(req)\n  return 42\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());

    HandlerOutcome o = s.call("pages/r.ms", "get.text");
    CHECK(o.kind == HandlerOutcome::Kind::Text);
    CHECK_EQ(o.body, "hello");

    CHECK(s.call("pages/r.ms", "get.none").kind == HandlerOutcome::Kind::NoContent);

    o = s.call("pages/r.ms", "get.go");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Redirect, describe(o));
    CHECK_EQ(o.location, "/thanks?x=1");

    o = s.call("pages/r.ms", "get.data");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Json, describe(o));
    CHECK_EQ(o.body, "{\"a\":[1,\"two\",null],\"b\":\"\\u003c/script\\u003e\"}");

    o = s.call("pages/r.ms", "get.gone");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Status, describe(o));
    CHECK_EQ(o.status, 410);
    CHECK_EQ(o.body, "gone");
    CHECK(contains(o.contentType, "text/plain"));

    o = s.call("pages/r.ms", "get.teapot");
    CHECK_EQ(o.status, 418);
    CHECK_EQ(o.body, "[1]");
    CHECK_EQ(o.contentType, "application/json");

    o = s.call("pages/r.ms", "get.empty");
    CHECK_EQ(o.status, 404);
    CHECK(o.body.empty());

    o = s.call("pages/r.ms", "get.oops");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK_EQ(o.error.message, "no such entry");
    CHECK_EQ(o.error.file, "code/pages/r.ms");

    o = s.call("pages/r.ms", "get.map");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "only render can"));

    o = s.call("pages/r.ms", "get.num");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "returned a number"));
}

TEST_CASE("ScriptHost: redirect, json and status refuse bad arguments") {
    Scripts s;
    s.write("pages/r.ms",
            "get.js = function(req)\n  return redirect(\"javascript:alert(1)\")\nend function\n"
            "get.slashes = function(req)\n  return redirect(\"//evil.example/\")\nend function\n"
            "get.backslash = function(req)\n  return redirect(\"/\\\\evil.example\")\nend function\n"
            "get.crlf = function(req)\n  return redirect(\"/a\" + char(13) + char(10) + \"Set-Cookie: x\")\nend function\n"
            "get.abs = function(req)\n  return redirect(\"https://example.com/x\")\nend function\n"
            "get.rel = function(req)\n  return redirect(\"other?a=1\")\nend function\n"
            "get.fn = function(req)\n  return json({\"f\": @print})\nend function\n"
            "get.cycle = function(req)\n  m = {}\n  m.me = m\n  return json(m)\nend function\n"
            "get.code = function(req)\n  return status(99)\nend function\n"
            "get.body = function(req)\n  return status(200, [1])\nend function\n"
            "get.forged = function(req)\n  return {\"__isa\": null, \"location\": \"/x\"}\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    for (const char* bad : {"get.js", "get.slashes", "get.backslash", "get.crlf", "get.fn", "get.cycle", "get.code",
                            "get.body", "get.forged"}) {
        CAPTURE(std::string(bad));
        CHECK(s.call("pages/r.ms", bad).kind == HandlerOutcome::Kind::Error);
    }
    CHECK_EQ(s.call("pages/r.ms", "get.abs").location, "https://example.com/x");
    CHECK_EQ(s.call("pages/r.ms", "get.rel").location, "other?a=1");
}

TEST_CASE("ScriptHost: req carries the request, and is frozen") {
    Scripts s;
    s.write("pages/q.ms",
            "get.echo = function(req)\n"
            "  return json([req.method, req.path, req.query.q, req.headers[\"user-agent\"], req.form.who])\n"
            "end function\n"
            "get.modify = function(req)\n  req.method = \"POST\"\n  return \"changed\"\nend function\n"
            "get.deep = function(req)\n  req.query.q = \"x\"\n  return \"changed\"\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());

    RequestFacts r;
    r.method = "GET";
    r.path = "/q/echo";
    r.query = {{"q", "caf\xC3\xA9"}, {"q", "second"}};
    r.headers = {{"user-agent", "test"}};
    r.form = {{"who", std::string("a\0b", 3)}};   // MS2 strings can't hold NUL
    HandlerOutcome o = s.call("pages/q.ms", "get.echo", r);
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Json, describe(o));
    CHECK_EQ(o.body, "[\"GET\",\"/q/echo\",\"caf\xC3\xA9\",\"test\",\"a\xEF\xBF\xBD" "b\"]");

    o = s.call("pages/q.ms", "get.modify", r);
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "frozen"));
    o = s.call("pages/q.ms", "get.deep", r);
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "frozen"));
}

TEST_CASE("ScriptHost: runtime errors carry file and line") {
    Scripts s;
    HandlerOutcome o = s.render("render = function(req)\n  x = 1\n  return nosuch.thing\nend function\n");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK_EQ(o.error.file, "code/pages/t.ms");
    CHECK_EQ(o.error.line, 3);
}

TEST_CASE("ScriptHost: each request starts fresh") {
    Scripts s;
    s.write("pages/c.ms",
            "count = 0\n"
            "get.bump = function(req)\n  globals.count = count + 1\n  return str(count)\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    CHECK_EQ(s.call("pages/c.ms", "get.bump").body, "1");
    CHECK_EQ(s.call("pages/c.ms", "get.bump").body, "1");
}

TEST_CASE("ScriptHost: shared built-in types can't be changed") {
    Scripts s;
    s.write("pages/e.ms",
            "get.patch = function(req)\n  string.shout = function\n    return self.upper\n  end function\n"
            "  return \"patched\"\nend function\n"
            "get.listPatch = function(req)\n  list.evil = 1\n  return \"patched\"\nend function\n"
            "get.use = function(req)\n  return \"hi\".upper\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    CHECK(s.call("pages/e.ms", "get.patch").kind == HandlerOutcome::Kind::Error);
    CHECK(s.call("pages/e.ms", "get.listPatch").kind == HandlerOutcome::Kind::Error);
    CHECK_EQ(s.call("pages/e.ms", "get.use").body, "HI");
}

TEST_CASE("ScriptHost: built-ins that don't belong in a request are disabled") {
    Scripts s;
    s.write("pages/d.ms",
            "get.input = function(req)\n  return input(\"?\")\nend function\n"
            "get.reset = function(req)\n  reset\n  return \"reset\"\nend function\n"
            "get.gc = function(req)\n  gc.collect\n  return \"collected\"\nend function\n"
            "get.history = function(req)\n  return _in\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    for (const char* h : {"get.input", "get.reset", "get.gc", "get.history"}) {
        CAPTURE(std::string(h));
        HandlerOutcome o = s.call("pages/d.ms", h);
        CHECK(o.kind == HandlerOutcome::Kind::Error);
        CHECK(contains(o.error.message, "isn't available in Fourfold"));
    }
}

TEST_CASE("ScriptHost: import loads only from code/lib/") {
    Scripts s;
    s.write("lib/util.ms", "double = function(x)\n  return x * 2\nend function\nname = \"util\"\n");
    s.write("lib/sub/deep.ms", "x = 1\n");
    s.write("pages/i.ms",
            "import \"util\"\n"
            "get.ok = function(req)\n  return str(util.double(21)) + \" \" + util.name\nend function\n"
            "get.again = function(req)\n  import \"util\"\n  return str(util.double(2))\nend function\n"
            "get.missing = function(req)\n  import \"nope\"\n  return \"imported\"\nend function\n"
            "get.dots = function(req)\n  import \"../pages/i\"\n  return \"imported\"\nend function\n"
            "get.slash = function(req)\n  import \"sub/deep\"\n  return \"imported\"\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    HandlerOutcome o = s.call("pages/i.ms", "get.ok");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Text, describe(o));
    CHECK_EQ(o.body, "42 util");
    CHECK_EQ(s.call("pages/i.ms", "get.again").body, "4");
    for (const char* h : {"get.missing", "get.dots", "get.slash"}) {
        CAPTURE(std::string(h));
        CHECK(s.call("pages/i.ms", h).kind == HandlerOutcome::Kind::Error);
    }
}

TEST_CASE("ScriptHost: print goes to the host's output, with the file") {
    Scripts s;
    HandlerOutcome o = s.render("print \"top\"\nrender = function(req)\n  print \"hi\"\n  return {}\nend function\n");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Render, describe(o));
    std::lock_guard<std::mutex> lock(s.printedMutex);
    CHECK(std::find(s.printed.begin(), s.printed.end(), "code/pages/t.ms: hi") != s.printed.end());
}

// --- Budgets (R3) ---

TEST_CASE("ScriptHost: an endless loop ends in an error, while other requests are served") {
    Scripts s;
    s.write("pages/b.ms",
            "get.forever = function(req)\n  while true\n  end while\nend function\n"
            "get.quick = function(req)\n  return \"quick\"\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    auto forever = s.start("pages/b.ms", "get.forever");
    auto t0 = SteadyClock::now();
    HandlerOutcome quick = s.call("pages/b.ms", "get.quick");
    auto quickTime = SteadyClock::now() - t0;
    CHECK_EQ(quick.body, "quick");
    CHECK(quickTime < std::chrono::milliseconds(500));
    REQUIRE(forever.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    HandlerOutcome o = forever.get();
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "instructions"));
}

TEST_CASE("ScriptHost: deep recursion and huge allocations end in errors") {
    Scripts s;
    s.write("pages/b.ms",
            "f = function(n)\n  return f(n + 1)\nend function\n"
            "get.recurse = function(req)\n  return f(0)\nend function\n"
            "get.bigString = function(req)\n  return \"a\" * 1e9\nend function\n"
            "get.bigList = function(req)\n  return [0] * 1e9\nend function\n"
            "get.growing = function(req)\n  s = \"ab\"\n  while true\n    s = s + s\n  end while\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    HandlerOutcome o = s.call("pages/b.ms", "get.recurse");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "overflow"));
    for (const char* h : {"get.bigString", "get.bigList", "get.growing"}) {
        CAPTURE(std::string(h));
        o = s.call("pages/b.ms", h);
        CHECK_MESSAGE(o.kind == HandlerOutcome::Kind::Error, describe(o));
    }
}

TEST_CASE("ScriptHost: the wall-clock budget includes time parked") {
    ScriptHostOptions options = Scripts::testOptions();
    options.budget.wallClock = std::chrono::milliseconds(200);
    Scripts s(options);
    HandlerOutcome o = s.render("render = function(req)\n  wait 5\n  return {}\nend function\n");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "time budget"));
}

// --- Parking (R2) ---

TEST_CASE("ScriptHost: wait parks the VM instead of blocking the thread") {
    Scripts s;
    s.write("pages/w.ms",
            "get.nap = function(req)\n  t = time\n  wait 0.2\n  return str(time - t >= 0.19)\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    const int kRuns = 200;
    std::vector<std::future<HandlerOutcome>> runs;
    auto t0 = SteadyClock::now();
    for (int i = 0; i < kRuns; i++) runs.push_back(s.start("pages/w.ms", "get.nap"));
    for (auto& f : runs) {
        REQUIRE(f.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        HandlerOutcome o = f.get();
        CHECK_EQ(o.body, "1");
    }
    // 200 naps of 0.2 s, overlapping: far less than 40 s.
    CHECK(SteadyClock::now() - t0 < std::chrono::seconds(2));
}

TEST_CASE("ScriptHost: many VMs parked on the worker pool, and the thread stays responsive") {
    ScriptHostOptions options = Scripts::testOptions();
    options.workerThreads = 64;
    options.budget.wallClock = std::chrono::seconds(20);
    Scripts s(options);
    s.write("pages/p.ms",
            "get.slow = function(req)\n  return _testSlowWork(100, \"done\")\nend function\n"
            "get.quick = function(req)\n  return \"quick\"\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    const int kParked = 1000;
    std::vector<std::future<HandlerOutcome>> slow;
    for (int i = 0; i < kParked; i++) slow.push_back(s.start("pages/p.ms", "get.slow"));

    // Wait until they're parked, then time a quick request among them.
    for (int tries = 0; tries < 200 && s.host->stats(false).parkedRuns < 100; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(s.host->stats(false).parkedRuns >= 100);
    auto t0 = SteadyClock::now();
    CHECK_EQ(s.call("pages/p.ms", "get.quick").body, "quick");
    CHECK(SteadyClock::now() - t0 < std::chrono::milliseconds(200));

    for (auto& f : slow) {
        REQUIRE(f.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
        CHECK_EQ(f.get().body, "done");
    }
    CHECK_EQ(s.host->stats(false).activeRuns, 0u);
}

TEST_CASE("ScriptHost: an intrinsic can wait, then call back into script, nested") {
    Scripts s;
    s.write("pages/cb.ms",
            "get.once = function(req)\n"
            "  return str(_testWithValue(20, function(x)\n    return x + 1\n  end function))\n"
            "end function\n"
            "get.nested = function(req)\n"
            "  inner = function(y)\n    return y * 10\n  end function\n"
            "  wrap = function(x)\n    return _testWithValue(x + 1, @inner) + _testSlowWork(10, 5)\n  end function\n"
            "  return str(_testWithValue(1, @wrap))\n"
            "end function\n"
            "get.closure = function(req)\n"
            "  base = 100\n"
            "  return str(_testWithValue(1, function(x)\n    return x + base\n  end function))\n"
            "end function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    HandlerOutcome o = s.call("pages/cb.ms", "get.once");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Text, describe(o));
    CHECK_EQ(o.body, "21");
    o = s.call("pages/cb.ms", "get.nested");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Text, describe(o));
    CHECK_EQ(o.body, "25");   // inner(2) = 20, plus 5
    o = s.call("pages/cb.ms", "get.closure");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Text, describe(o));
    CHECK_EQ(o.body, "101");
}

TEST_CASE("ScriptHost: test intrinsics are off unless asked for") {
    ScriptHostOptions options = Scripts::testOptions();
    options.testIntrinsics = false;
    Scripts s(options);
    HandlerOutcome o = s.render("render = function(req)\n  return _testSlowWork(1, 2)\nend function\n");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "only available in tests"));
}

// --- Memory ---

TEST_CASE("ScriptHost: runs leave nothing behind") {
    Scripts s;
    s.write("pages/m.ms",
            "helper = function(x)\n  return x\nend function\n"
            "render = function(req)\n  junk = []\n  for i in range(100)\n    junk.push({\"i\": i})\n  end for\n"
            "  return {\"n\": helper(junk.len)}\nend function\n"
            "get.err = function(req)\n  junk = [{}, {}]\n  return nosuch\nend function\n"
            "get.park = function(req)\n  junk = [{}, {}]\n  return _testSlowWork(1, \"ok\")\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    s.call("pages/m.ms", "render");   // warm up (e.g. lazily built intrinsics)
    ScriptHost::Stats before = s.host->stats();
    for (int i = 0; i < 300; i++) {
        s.call("pages/m.ms", "render");
        s.call("pages/m.ms", "get.err");
        s.call("pages/m.ms", "get.park");
    }
    ScriptHost::Stats after = s.host->stats();
    CHECK(after.liveMaps <= before.liveMaps + 5);
    CHECK(after.liveLists <= before.liveLists + 5);
    CHECK(after.liveFunctions <= before.liveFunctions + 5);
}

TEST_CASE("ScriptHost: a compiled app keeps its bytecode alive until released") {
    Scripts s;
    HandlerOutcome o = s.render(
        "big = \"" + std::string(200, 'x') + "\"\n"
        "render = function(req)\n  return {\"len\": big.len}\nend function\n",
        {}, "<!DOCTYPE html>{{len}}");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Render, describe(o));
    s.host->stats();   // a full collection
    o = s.call("pages/t.ms", "render");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Render, describe(o));
    CHECK_EQ(o.body, "<!DOCTYPE html>200");

    int functionsWithApp = s.host->stats().liveFunctions;
    s.app.reset();
    s.compiled = {};
    int functionsWithout = s.host->stats().liveFunctions;
    CHECK(functionsWithout < functionsWithApp);
}

TEST_CASE("ScriptHost: requests are served while a long compile runs") {
    Scripts s;
    s.write("pages/q.ms", "get.quick = function(req)\n  return \"quick\"\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());

    // Many pages whose top levels each take a while.
    for (int i = 0; i < 20; i++) {
        s.write("pages/slow" + std::to_string(i) + ".ms",
                "n = 0\nfor i in range(30000)\n  n = n + i\nend for\n"
                "render = function(req)\n  return {\"n\": n}\nend function\n");
    }
    std::vector<CodeFile> pages, libs;
    s.readFiles(pages, libs);
    auto t0 = SteadyClock::now();
    auto compiling = std::async(std::launch::async, [&] { return s.host->compileScripts(pages, libs); });
    SteadyClock::duration worst{};
    int served = 0;
    while (compiling.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        auto start = SteadyClock::now();
        HandlerOutcome o = s.call("pages/q.ms", "get.quick");
        worst = std::max(worst, SteadyClock::now() - start);
        CHECK_EQ(o.body, "quick");
        served++;
    }
    auto compileTime = SteadyClock::now() - t0;
    CompiledScripts result = compiling.get();
    CHECK_MESSAGE(result.bundle, "compile failed");
    CHECK_EQ(result.pages.size(), 21u);
    MESSAGE("compile took ", std::chrono::duration_cast<std::chrono::milliseconds>(compileTime).count(),
            " ms; ", served, " requests served meanwhile, the slowest in ",
            std::chrono::duration_cast<std::chrono::microseconds>(worst).count(), " us");
    CHECK(served > 5);
    CHECK(worst < compileTime / 4);
    CHECK(worst < std::chrono::milliseconds(100));
}

TEST_CASE("ScriptHost: bundles are counted until released") {
    Scripts s;
    int before = s.host->stats().liveBundles;
    HandlerOutcome o = s.render("render = function(req)\n  return {}\nend function\n");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Render, describe(o));
    CHECK_EQ(s.host->stats().liveBundles, before + 1);
    s.app.reset();
    s.compiled = {};
    CHECK_EQ(s.host->stats().liveBundles, before);
}

// --- HTML from code: frag, markdown, unsafeHTML (M4) ---

namespace {

// Runs get.x in pages/h.ms, with these fragments in code/frags/.
HandlerOutcome callWithFrags(Scripts& s, const std::string& body,
                             const std::vector<std::pair<std::string, std::string>>& frags,
                             RequestFacts request = {}, RenderOptions options = {}) {
    for (const auto& [name, text] : frags) s.write("frags/" + name + ".html", text);
    s.write("pages/h.ms", "get.x = function(req)\n" + body + "\nend function\n");
    s.compile();
    REQUIRE_MESSAGE(s.compiled.bundle, s.diagnostics());
    return s.call("pages/h.ms", "get.x", std::move(request), std::move(options));
}

}  // namespace

TEST_CASE("ScriptHost: frag renders a fragment") {
    Scripts s;
    std::vector<std::pair<std::string, std::string>> frags = {{"entry", "<li>{{who}}</li>"},
                                                              {"empty", "<hr>"}};
    HandlerOutcome o = callWithFrags(s, "return frag(\"entry\", {\"who\": \"<b>Ann</b>\"})", frags);
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Fragment, describe(o));
    CHECK_EQ(o.body, "<li>&lt;b&gt;Ann&lt;/b&gt;</li>");
    CHECK_FALSE(o.hasScript);

    o = s.call("pages/h.ms", "get.x");   // same again
    CHECK_EQ(o.body, "<li>&lt;b&gt;Ann&lt;/b&gt;</li>");

    s.write("pages/h.ms", "get.x = function(req)\n  return frag(\"entry\", [{\"who\": 1}, {\"who\": \"b\"}, null])\n"
                          "end function\n");
    s.compile();
    o = s.call("pages/h.ms", "get.x");
    CHECK_EQ(o.body, "<li>1</li><li>b</li><li></li>");

    s.write("pages/h.ms", "get.x = function(req)\n  return frag(\"empty\")\nend function\n");
    s.compile();
    CHECK_EQ(s.call("pages/h.ms", "get.x").body, "<hr>");
}

TEST_CASE("ScriptHost: frag's errors") {
    Scripts s;
    std::vector<std::pair<std::string, std::string>> frags = {{"entry", "<li>{{who}}</li>"}};
    struct Case {
        const char* code;
        const char* message;
    };
    const Case cases[] = {
        {"return frag(\"nope\")", "there's no code/frags/nope.html"},
        {"return frag(42)", "frag needs a fragment's name"},
        {"return frag(\"entry\", \"text\")", "the value must be a map of fields"},
        {"return frag(\"entry\", [{\"who\": 1}, \"x\"])", "item 1 isn't a map"},
        {"return frag(\"entry\", {\"who\": [1]})", "{{who}} is a list"},
    };
    for (const Case& c : cases) {
        INFO(c.code);
        HandlerOutcome o = callWithFrags(s, std::string("  ") + c.code, frags);
        REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Error, describe(o));
        CHECK_MESSAGE(contains(o.error.message, c.message), o.error.message);
        CHECK_EQ(o.error.file, "code/pages/h.ms");
    }
}

TEST_CASE("ScriptHost: Markdown fragments take no value") {
    Scripts s;
    s.write("pages/h.ms", "get.x = function(req)\n  return frag(\"intro\", {\"a\": 1})\nend function\n");
    s.compile();
    FragmentMap frags;
    frags["intro"] = CompiledTemplate::fromMarkdown(RelPath::parse("frags/intro.md").value(), unsafeHtml("<p>hi</p>"));
    s.app->templates.setFragments(std::move(frags));
    HandlerOutcome o = s.call("pages/h.ms", "get.x");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "is Markdown, which has no fields"));
    s.write("pages/h.ms", "get.x = function(req)\n  return frag(\"intro\")\nend function\n");
    s.compile();
    frags["intro"] = CompiledTemplate::fromMarkdown(RelPath::parse("frags/intro.md").value(), unsafeHtml("<p>hi</p>"));
    s.app->templates.setFragments(std::move(frags));
    CHECK_EQ(s.call("pages/h.ms", "get.x").body, "<p>hi</p>");
}

TEST_CASE("ScriptHost: rendered HTML goes into a page's fields") {
    Scripts s;
    s.write("frags/entry.html", "<li>{{who}}</li>");
    RenderOptions options;
    options.nonce = "N0nce";
    s.write("pages/t.ms", "render = function(req)\n"
                          "  return {\"list\": frag(\"entry\", [{\"who\": \"a\"}]), \"md\": markdown(\"*hi*\"), "
                          "\"raw\": unsafeHTML(\"<i>raw</i>\"), \"cfg\": {\"a\": [1, \"</script>\"]}}\n"
                          "end function\n");
    s.write("pages/t.html", "<!DOCTYPE html><html><head></head><body><ul>{{list}}</ul>{{md}}{{raw}}"
                            "<script>go({{cfg}})</script></body></html>");
    s.compile();
    options.markdownCss = "/m.css";
    HandlerOutcome o = s.call("pages/t.ms", "render", {}, options);
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Render, describe(o));
    CHECK_EQ(o.body,
             "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"/m.css\">\n</head><body><ul><li>a</li></ul>"
             "<div class=\"markdown comment\">\n<p><em>hi</em></p>\n</div>\n<i>raw</i>"
             "<script nonce=\"N0nce\">go({\"a\":[1,\"\\u003c\\u002fscript\\u003e\"]})</script></body></html>");
}

TEST_CASE("ScriptHost: only handles are HTML; nothing a script builds can pass for one") {
    Scripts s;
    const char* forgeries[] = {
        "{\"html\": \"<b>x</b>\"}",
        "{\"__isa\": {\"html\": \"<b>x</b>\"}}",
        "\"<b>x</b>\"",
    };
    for (const char* forged : forgeries) {
        INFO(forged);
        HandlerOutcome o = s.render(std::string("render = function(req)\n  return {\"x\": ") + forged +
                                        "}\nend function\n",
                                    {}, "<!DOCTYPE html>{{x}}");
        if (o.kind == HandlerOutcome::Kind::Render) {
            CHECK(o.body.find("<b>") == std::string::npos);
        } else {
            CHECK(contains(o.error.message, "is a map"));
        }
    }
    // A plain string returned is text/plain, never HTML.
    s.write("pages/h.ms", "get.x = function(req)\n  return \"<b>x</b>\"\nend function\n");
    s.compile();
    CHECK_EQ(s.call("pages/h.ms", "get.x").kind, HandlerOutcome::Kind::Text);
    // Html can't be turned into JSON, nor stored.
    s.write("pages/h.ms", "get.x = function(req)\n  return json({\"h\": markdown(\"x\")})\nend function\n");
    s.compile();
    HandlerOutcome o = s.call("pages/h.ms", "get.x");
    REQUIRE(o.kind == HandlerOutcome::Kind::Error);
    CHECK(contains(o.error.message, "handle"));
}

TEST_CASE("ScriptHost: markdown()") {
    Scripts s;
    auto md = [&](const std::string& call) {
        s.write("pages/h.ms", "get.x = function(req)\n  return " + call + "\nend function\n");
        s.compile();
        return s.call("pages/h.ms", "get.x");
    };
    HandlerOutcome o = md("markdown(\"# Title\")");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Fragment, describe(o));
    CHECK_EQ(o.body, "<div class=\"markdown comment\">\n<p>Title</p>\n</div>\n");
    CHECK_EQ(md("markdown(\"# Title\", \"full\")").body, "<div class=\"markdown\">\n<h1 id=\"title\">Title</h1>\n</div>\n");
    CHECK_EQ(md("markdown(null)").body, "<div class=\"markdown comment\">\n</div>\n");
    CHECK(contains(md("markdown(\"x\", \"raw\")").error.message, "profile must be"));
    CHECK(contains(md("markdown(42)").error.message, "needs a string"));
    CHECK(contains(md("markdown(\"x\" * 300000)").error.message, "over the limit"));
}

TEST_CASE("ScriptHost: unsafeHTML() and fragments with scripts") {
    Scripts s;
    std::vector<std::pair<std::string, std::string>> frags = {{"js", "<script>x()</script>"}};
    HandlerOutcome o = callWithFrags(s, "  return unsafeHTML(\"<b>trusted</b>\")", frags);
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Fragment, describe(o));
    CHECK_EQ(o.body, "<b>trusted</b>");
    CHECK_FALSE(o.hasScript);
    CHECK(callWithFrags(s, "  return unsafeHTML(\"<SCRIPT>x()</script>\")", frags).hasScript);
    CHECK(callWithFrags(s, "  return frag(\"js\")", frags).hasScript);
    o = callWithFrags(s, "  return status(422, frag(\"js\"))", frags);
    CHECK_EQ(o.kind, HandlerOutcome::Kind::Status);
    CHECK(o.hasScript);
    CHECK_EQ(o.contentType, "text/html; charset=utf-8");
    CHECK(contains(callWithFrags(s, "  return unsafeHTML(1)", frags).error.message, "needs a string"));
}

TEST_CASE("ScriptHost: req.htmx") {
    Scripts s;
    RequestFacts htmx;
    htmx.headers = {{"hx-request", "true"}};
    std::vector<std::pair<std::string, std::string>> none;
    CHECK_EQ(callWithFrags(s, "  return str(req.htmx)", none, htmx).body, "1");
    CHECK_EQ(callWithFrags(s, "  return str(req.htmx)", none).body, "0");
}

TEST_CASE("ScriptHost: missing and unused fields are warned about") {
    Scripts s;
    s.write("frags/entry.html", "<li>{{who}} {{when}}</li>");
    s.write("pages/t.ms", "render = function(req)\n"
                          "  return {\"list\": frag(\"entry\", {\"who\": 1}), \"typo\": 2}\n"
                          "end function\n");
    s.write("pages/t.html", "<!DOCTYPE html>{{list}} {{title}}");
    s.compile();
    HandlerOutcome o = s.call("pages/t.ms", "render");
    REQUIRE_MESSAGE(o.kind == HandlerOutcome::Kind::Render, describe(o));
    std::string all;
    for (const RenderWarning& w : o.warnings) all += w.file + ":" + std::to_string(w.line) + ": " + w.message + "\n";
    CHECK_MESSAGE(contains(all, "frags/entry.html:1: {{when}} wasn't given a value"), all);
    CHECK_MESSAGE(contains(all, "pages/t.html:1: {{title}} wasn't given a value"), all);
    CHECK_MESSAGE(contains(all, "\"typo\" was given a value"), all);
}

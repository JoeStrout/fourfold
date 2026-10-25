#include "devtools/dev_tools.h"

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;
using std::chrono::milliseconds;

namespace {

RelPath rel(const char* p) { return RelPath::parse(p).value(); }

CodeTree codeTree(const TempDir& dir) {
    auto folder = AppFolder::open(dir.path(), AppFolder::Missing::Create);
    REQUIRE(folder.ok());
    auto code = folder.value().codeTree();
    REQUIRE(code.ok());
    return std::move(code).value();
}

struct FakeWriter : StreamWriter {
    std::string written;
    bool open = true;
    bool write(std::string bytes) override {
        if (!open) return false;
        written += bytes;
        return true;
    }
    void close() override { open = false; }
    bool isOpen() const override { return open; }
};

struct Fixture {
    TempDir dir;
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<DevTools> dev;

    Fixture() {
        dir.write("code/pages/index.ms", "line 1\nline 2\nline 3 <script>\nline 4\nline 5\nline 6\nline 7\nline 8\n");
        auto l = EventLoop::create();
        REQUIRE(l.ok());
        loop = std::move(l).value();
        dev = std::make_unique<DevTools>(*loop, codeTree(dir));
    }

    std::optional<HttpResponse> get(const std::string& path, const char* method = "GET") {
        HttpRequest request;
        request.method = method;
        request.target = path;
        AdmittedRequest admitted;
        admitted.method = method;
        admitted.path = canonicalUrlPath(path).value();
        return dev->reserved(request, admitted);
    }

    // Opens /__dev/reload, as the server would after sending its headers.
    std::shared_ptr<FakeWriter> connect() {
        auto r = get("/__dev/reload");
        REQUIRE(r);
        REQUIRE(r->stream);
        auto writer = std::make_shared<FakeWriter>();
        r->stream(writer);
        return writer;
    }

    void pump() {
        for (int i = 0; i < 3; i++) loop->runOnce(milliseconds(1));
    }
};

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("insertBeforeBodyEnd") {
    std::string a = "<html><body><p>x</p></BODY></html>";
    insertBeforeBodyEnd(a, "<s>");
    CHECK_EQ(a, "<html><body><p>x</p><s></BODY></html>");
    std::string b = "<p>no body</p>";
    insertBeforeBodyEnd(b, "<s>");
    CHECK_EQ(b, "<p>no body</p><s>");
    std::string c = "<body><p>&lt;/body&gt; in text</p></body>";
    insertBeforeBodyEnd(c, "<s>");
    CHECK_EQ(c, "<body><p>&lt;/body&gt; in text</p><s></body>");
}

TEST_CASE("DevTools: decorates pages with the client script and their generation") {
    Fixture f;
    HttpResponse r;
    r.body = "<html><body>hi</body></html>";
    f.dev->decorate(r, 12);
    CHECK_EQ(r.body, "<html><body>hi<script src=\"/__dev/client.js\" data-generation=\"12\" defer></script></body></html>");
}

TEST_CASE("DevTools: serves the client script, and nothing else under /__dev/") {
    Fixture f;
    auto js = f.get("/__dev/client.js");
    REQUIRE(js);
    CHECK_EQ(*js->headers.get("Content-Type"), "text/javascript; charset=utf-8");
    CHECK(contains(js->body, "EventSource(\"/__dev/reload\")"));
    CHECK_FALSE(f.get("/__dev/other"));
    CHECK_FALSE(f.get("/__dev/client.js", "POST"));
}

TEST_CASE("DevTools: the reload stream") {
    Fixture f;
    auto r = f.get("/__dev/reload");
    REQUIRE(r);
    CHECK_EQ(*r->headers.get("Content-Type"), "text/event-stream");

    auto writer = f.connect();
    CHECK_EQ(writer->written, "event: hello\ndata: {\"diagnostics\":[],\"generation\":0}\n\n");
    CHECK_EQ(f.dev->streamCount(), 1u);
    writer->written.clear();

    SUBCASE("a successful compile: reload") {
        CompileReport report;
        report.ok = true;
        report.generation = 2;
        report.changed = {rel("pages/index.ms")};
        f.dev->compileFinished(report);
        f.pump();
        CHECK_EQ(writer->written, "event: reload\ndata: {\"generation\":2}\n\n");
        auto later = f.connect();
        CHECK_EQ(later->written, "event: hello\ndata: {\"diagnostics\":[],\"generation\":2}\n\n");
    }
    SUBCASE("only stylesheets: css, with their URLs") {
        CompileReport report;
        report.ok = true;
        report.cssOnly = true;
        report.generation = 3;
        report.changed = {rel("pages/style.css"), rel("pages/a b/x.css")};
        f.dev->compileFinished(report);
        f.pump();
        CHECK_EQ(writer->written,
                 "event: css\ndata: {\"generation\":3,\"urls\":[\"/style.css\",\"/a%20b/x.css\"]}\n\n");
    }
    SUBCASE("a failed compile: errors, with the code around each; and new streams hear them") {
        CompileReport report;
        report.ok = false;
        report.generation = 1;
        report.diagnostics = {{Diagnostic::Severity::Error, "code/pages/index.ms", 3, "bad <thing>"},
                              {Diagnostic::Severity::Warning, "code/pages/index.ms", 1, "just a warning"},
                              {Diagnostic::Severity::Error, "code", 0, "no line"}};
        f.dev->compileFinished(report);
        f.pump();
        // '<' and '>' are escaped by the JSON encoder.
        CHECK_EQ(writer->written,
                 "event: errors\ndata: {\"diagnostics\":["
                 "{\"code\":[\"line 1\",\"line 2\",\"line 3 \\u003cscript\\u003e\",\"line 4\",\"line 5\",\"line 6\"],"
                 "\"file\":\"code/pages/index.ms\",\"first\":1,\"line\":3,\"message\":\"bad \\u003cthing\\u003e\"},"
                 "{\"file\":\"code\",\"line\":0,\"message\":\"no line\"}],\"generation\":1}\n\n");
        auto later = f.connect();
        CHECK(contains(later->written, "event: hello\ndata: {\"diagnostics\":[{\"code\""));
        CHECK(contains(later->written, "\"generation\":1}"));
    }
    SUBCASE("closed streams are dropped") {
        writer->open = false;
        CompileReport report;
        report.ok = true;
        f.dev->compileFinished(report);
        f.pump();
        CHECK_EQ(f.dev->streamCount(), 0u);
    }
}

TEST_CASE("DevTools: the script error page escapes everything, and shows the code") {
    Fixture f;
    HttpResponse r = f.dev->scriptErrorPage({"</p><script>alert(1)</script>", "code/pages/index.ms", 3});
    CHECK_EQ(r.status, 500);
    CHECK_EQ(*r.headers.get("Content-Type"), "text/html; charset=utf-8");
    CHECK_FALSE(contains(r.body, "<script>"));
    CHECK(contains(r.body, "&lt;/p&gt;&lt;script&gt;alert(1)&lt;/script&gt;"));
    CHECK(contains(r.body, "code/pages/index.ms, line 3"));
    CHECK(contains(r.body, "<span class=\"hit\"><i>3</i>line 3 &lt;script&gt;</span>"));
    CHECK(contains(r.body, "<i>6</i>line 6"));
    CHECK_FALSE(contains(r.body, "line 7"));

    SUBCASE("files outside code/, or missing, show no code") {
        HttpResponse other = f.dev->scriptErrorPage({"oops", "/etc/passwd", 1});
        CHECK_FALSE(contains(other.body, "<pre>"));
        HttpResponse missing = f.dev->scriptErrorPage({"oops", "code/pages/nope.ms", 1});
        CHECK_FALSE(contains(missing.body, "<pre>"));
        HttpResponse dots = f.dev->scriptErrorPage({"oops", "code/../code/pages/index.ms", 1});
        CHECK_FALSE(contains(dots.body, "<pre>"));
    }
}

#include "compiledapp/compiled_app.h"

#include "doctest.h"

using namespace fourfold;

TEST_CASE("RouteTable: exact lookups, no duplicates") {
    RouteTable t;
    RelPath about = RelPath::parse("pages/about.html").value();
    CHECK(t.add("/about", {RouteKind::Page, about}));
    CHECK_FALSE(t.add("/about", {RouteKind::Static, about}));
    REQUIRE(t.find("/about") != nullptr);
    CHECK(t.find("/about")->kind == RouteKind::Page);
    CHECK(t.find("/About") == nullptr);
    CHECK(t.find("/about/") == nullptr);
    CHECK_EQ(t.size(), 1u);
}

TEST_CASE("describe and Diagnostic::toString") {
    CompiledApp app;
    RelPath css = RelPath::parse("pages/style.css").value();
    app.routes.add("/style.css", {RouteKind::Static, css});
    app.statics.add(css, {"text/css; charset=utf-8", 10, 0});
    app.routes.add("/", {RouteKind::Page, RelPath::parse("pages/index.html").value()});
    app.diagnostics.push_back({Diagnostic::Severity::Warning, "code/pages/x.xyz", 0, "unknown"});
    CHECK_EQ(describe(app),
             "routes:\n"
             "  /  page  pages/index.html\n"
             "  /style.css  static  pages/style.css  text/css; charset=utf-8\n"
             "not found page: (none)\n"
             "error page: (none)\n"
             "diagnostics:\n"
             "  code/pages/x.xyz: warning: unknown\n");

    RouteEntry handler{RouteKind::Handler, RelPath::parse("pages/g.ms").value()};
    handler.get = HandlerRef{HandlerRef::Kind::Get, handler.source, "more"};
    handler.post = HandlerRef{HandlerRef::Kind::Post, handler.source, "more"};
    app.routes.add("/g/more", handler);
    CHECK(describe(app).find("  /g/more  handler  pages/g.ms  get.more in pages/g.ms  post.more in pages/g.ms\n") !=
          std::string::npos);

    Diagnostic d{Diagnostic::Severity::Error, "code/pages/a.html", 3, "bad"};
    CHECK_EQ(d.toString(), "code/pages/a.html:3: error: bad");
}

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
             "diagnostics:\n"
             "  code/pages/x.xyz: warning: unknown\n");

    Diagnostic d{Diagnostic::Severity::Error, "code/pages/a.html", 3, "bad"};
    CHECK_EQ(d.toString(), "code/pages/a.html:3: error: bad");
}

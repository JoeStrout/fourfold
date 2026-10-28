#include "authflow/auth_flow.h"

#include "doctest.h"

using namespace fourfold;

TEST_CASE("AuthFlow: where to go after signing in stays on this site") {
    CHECK_EQ(AuthFlow::safeNext("/members/"), "/members/");
    CHECK_EQ(AuthFlow::safeNext("/board?page=2"), "/board?page=2");
    for (const char* bad : {"", "members", "//evil.example/", "/\\evil.example", "https://evil.example/",
                            "/x\r\nSet-Cookie: a=b", "/__auth/logout", "/__auth"}) {
        CAPTURE(bad);
        CHECK_EQ(AuthFlow::safeNext(bad), "/");
    }
}

TEST_CASE("AuthFlow: the fake login offers a user per role the rules name") {
    CompiledApp app;
    RouteEntry mod{RouteKind::Page, RelPath::parse("pages/mod.html").value()};
    mod.access.require({"moderator", "admin"});
    app.routes.add("/mod", mod);
    RouteEntry pay{RouteKind::Page, RelPath::parse("pages/pay.html").value()};
    pay.access.require({"accountant"});
    app.routes.add("/pay", pay);

    std::vector<std::string> keys;
    for (const FakeUser& u : AuthFlow::fakeUsers(&app)) keys.push_back(u.key);
    CHECK_EQ(keys, std::vector<std::string>{"admin", "member", "accountant", "moderator"});
    CHECK_EQ(AuthFlow::fakeUsers(nullptr).size(), 2u);
    CHECK_EQ(AuthFlow::fakeUsers(&app)[3].roles, std::vector<std::string>{"moderator"});
}

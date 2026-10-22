#include "http/message.h"

#include "doctest.h"

using namespace fourfold;

TEST_CASE("Headers: lookup ignores case, and keeps the first match") {
    Headers h;
    h.add("Content-Type", "text/html");
    h.add("Set-Cookie", "a=1");
    h.add("set-cookie", "b=2");
    REQUIRE(h.get("content-type") != nullptr);
    CHECK_EQ(*h.get("CONTENT-TYPE"), "text/html");
    CHECK_EQ(*h.get("Set-Cookie"), "a=1");
    CHECK_EQ(h.size(), 3u);
    CHECK_FALSE(h.has("X-Missing"));
}

TEST_CASE("Headers: set replaces every field of that name, and remove deletes them") {
    Headers h;
    h.add("Set-Cookie", "a=1");
    h.add("SET-COOKIE", "b=2");
    h.add("Vary", "Accept");
    h.set("set-cookie", "c=3");
    CHECK_EQ(h.size(), 2u);
    CHECK_EQ(*h.get("Set-Cookie"), "c=3");
    h.remove("vary");
    CHECK_EQ(h.size(), 1u);
    CHECK_FALSE(h.has("Vary"));
}

TEST_CASE("equalsIgnoreCase") {
    CHECK(equalsIgnoreCase("Keep-Alive", "keep-alive"));
    CHECK_FALSE(equalsIgnoreCase("Keep-Alive", "keep-alive "));
    CHECK(equalsIgnoreCase("", ""));
}

TEST_CASE("HttpResponse::text and statusReason") {
    HttpResponse r = HttpResponse::text(404, "nope");
    CHECK_EQ(r.status, 404);
    CHECK_EQ(r.body, "nope");
    CHECK_EQ(*r.headers.get("Content-Type"), "text/plain; charset=utf-8");
    CHECK_EQ(std::string(statusReason(404)), "Not Found");
    CHECK_EQ(std::string(statusReason(431)), "Request Header Fields Too Large");
    CHECK_EQ(std::string(statusReason(299)), "Unknown");
}

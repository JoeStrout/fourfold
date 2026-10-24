#include "base/datum.h"

#include "doctest.h"

using fourfold::Datum;
using fourfold::toJson;

TEST_CASE("Datum: kinds and values") {
    CHECK(Datum().isNull());
    CHECK(Datum::number(2.5).kind() == Datum::Kind::Number);
    CHECK_EQ(Datum::number(2.5).asNumber(), 2.5);
    CHECK_EQ(Datum::string("hi").asString(), "hi");
    Datum list = Datum::list({Datum::number(1), Datum()});
    REQUIRE(list.kind() == Datum::Kind::List);
    CHECK_EQ(list.asList().size(), 2u);
    Datum map = Datum::map({{"b", Datum::number(2)}, {"a", Datum::string("x")}});
    REQUIRE(map.kind() == Datum::Kind::Map);
    CHECK_EQ(map.asMap().begin()->first, "a");   // keys sorted
}

TEST_CASE("Datum: equality compares contents") {
    CHECK(Datum() == Datum());
    CHECK(Datum::number(1) == Datum::number(1));
    CHECK(Datum::number(1) != Datum::string("1"));
    CHECK(Datum::list({Datum::number(1)}) == Datum::list({Datum::number(1)}));
    CHECK(Datum::map({{"a", Datum()}}) != Datum::map({{"b", Datum()}}));
    CHECK(Datum() != Datum::number(0));
}

TEST_CASE("toJson: scalars") {
    CHECK_EQ(toJson(Datum()), "null");
    CHECK_EQ(toJson(Datum::number(3)), "3");
    CHECK_EQ(toJson(Datum::number(-0.0)), "0");
    CHECK_EQ(toJson(Datum::number(0.1)), "0.1");
    CHECK_EQ(toJson(Datum::number(1e300)), "1e+300");
    CHECK_EQ(toJson(Datum::number(123456789012.0)), "123456789012");
    CHECK_EQ(toJson(Datum::number(1.0 / 3)), "0.3333333333333333");
}

TEST_CASE("toJson: strings are escaped for JSON and for HTML <script>") {
    CHECK_EQ(toJson(Datum::string("a\"b\\c")), "\"a\\\"b\\\\c\"");
    CHECK_EQ(toJson(Datum::string("line\nnext\ttab")), "\"line\\nnext\\ttab\"");
    CHECK_EQ(toJson(Datum::string(std::string("\x01\x7f", 2))), "\"\\u0001\\u007f\"");
    CHECK_EQ(toJson(Datum::string("</script><b>&")), "\"\\u003c/script\\u003e\\u003cb\\u003e\\u0026\"");
    CHECK_EQ(toJson(Datum::string("caf\xC3\xA9")), "\"caf\xC3\xA9\"");
    CHECK_EQ(toJson(Datum::string("a\xE2\x80\xA8" "b\xE2\x80\xA9")), "\"a\\u2028b\\u2029\"");
}

TEST_CASE("toJson: lists and maps") {
    CHECK_EQ(toJson(Datum::list({})), "[]");
    CHECK_EQ(toJson(Datum::map({})), "{}");
    Datum d = Datum::map({{"z", Datum::list({Datum::number(1), Datum::string("two"), Datum()})},
                          {"a", Datum::map({{"k", Datum::number(0.5)}})}});
    CHECK_EQ(toJson(d), "{\"a\":{\"k\":0.5},\"z\":[1,\"two\",null]}");
}

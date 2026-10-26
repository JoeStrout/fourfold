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

TEST_CASE("parseJson: reads back what toJson writes") {
    using fourfold::parseJson;
    Datum value = Datum::map({
        {"n", Datum::number(-12.5)},
        {"tiny", Datum::number(1e-300)},
        {"s", Datum::string("a\"b\\c\n</script>\x01 \xE2\x80\xA8 \xF0\x9F\x98\x80 caf\xC3\xA9")},
        {"nul", Datum::string(std::string("a\0b", 3))},
        {"list", Datum::list({Datum(), Datum::number(0), Datum::list({}), Datum::map({})})},
    });
    auto back = parseJson(toJson(value));
    REQUIRE(back.ok());
    CHECK(back.value() == value);
    CHECK_EQ(toJson(back.value()), toJson(value));
    auto fromScript = parseJson(fourfold::toJsonForScript(value));
    REQUIRE(fromScript.ok());
    CHECK(fromScript.value() == value);
}

TEST_CASE("parseJson: accepts ordinary JSON") {
    using fourfold::parseJson;
    auto r = parseJson(" { \"b\" : [ 1 , 2.5e2 , -0 , true , false ] , \"a\" : \"\\u00e9\\ud83d\\ude00\\/\" } ");
    REQUIRE(r.ok());
    const auto& m = r.value().asMap();
    CHECK_EQ(m.at("a").asString(), "\xC3\xA9\xF0\x9F\x98\x80/");
    const auto& l = m.at("b").asList();
    CHECK_EQ(l[1].asNumber(), 250);
    CHECK_EQ(toJson(l[2]), "0");
    CHECK_EQ(l[3].asNumber(), 1);   // MiniScript's true
    CHECK_EQ(l[4].asNumber(), 0);
}

TEST_CASE("parseJson: refuses what isn't valid") {
    using fourfold::parseJson;
    const char* bad[] = {
        "", " ", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "{a:1}", "[1 2]", "01", "1.", ".5", "-", "1e",
        "+1", "NaN", "Infinity", "1e999", "\"abc", "\"\\x\"", "\"\\u12\"", "\"\\ud800\"", "\"\\udc00\"",
        "\"\\ud800\\u0041\"", "\"a\nb\"", "\"\xC0\xAE\"", "\"\xFF\"", "nul", "[1] 2", "{\"a\":1,\"a\":2}",
        "'a'",
    };
    for (const char* text : bad) {
        INFO(text);
        auto r = parseJson(text);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == fourfold::ErrorCode::Invalid);
    }
}

TEST_CASE("parseJson: limits") {
    using fourfold::parseJson;
    fourfold::JsonLimits limits;
    limits.maxDepth = 3;
    CHECK(parseJson("[[[1]]]", limits).ok());   // the 1 is at depth 3, as in the bridge
    CHECK_FALSE(parseJson("[[[[1]]]]", limits).ok());
    limits.maxBytes = 4;
    auto r = parseJson("[1,2]", limits);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == fourfold::ErrorCode::TooLarge);
    // The default depth matches what scripts can store.
    std::string deep(64, '[');
    deep += std::string(64, ']');
    CHECK(parseJson(deep).ok());
}

TEST_CASE("toJson: whole numbers are written whole") {
    CHECK_EQ(toJson(Datum::number(400)), "400");
    CHECK_EQ(toJson(Datum::number(-1000000)), "-1000000");
    CHECK_EQ(toJson(Datum::number(9007199254740992.0)), "9007199254740992");
    CHECK_EQ(toJson(Datum::number(1e16)), "1e+16");
    CHECK_EQ(toJson(Datum::number(2.5e-7)), "2.5e-07");
}

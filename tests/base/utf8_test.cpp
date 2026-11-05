#include "base/utf8.h"

#include <string>

#include "doctest.h"

using fourfold::isValidUtf8;

TEST_CASE("isValidUtf8 accepts well-formed text") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("plain ASCII"));
    CHECK(isValidUtf8("caf\xC3\xA9"));               // é
    CHECK(isValidUtf8("\xE2\x82\xAC"));              // €
    CHECK(isValidUtf8("\xF0\x9F\x98\x80"));          // 😀
    CHECK(isValidUtf8("\xED\x9F\xBF"));              // U+D7FF, just below the surrogates
    CHECK(isValidUtf8("\xF4\x8F\xBF\xBF"));          // U+10FFFF, the last code point
    CHECK(isValidUtf8(std::string("a\0b", 3)));      // NUL is valid UTF-8 (callers reject it)
}

TEST_CASE("isValidUtf8 rejects malformed text") {
    CHECK_FALSE(isValidUtf8("\x80"));                // stray continuation byte
    CHECK_FALSE(isValidUtf8("\xC3"));                // truncated
    CHECK_FALSE(isValidUtf8("\xE2\x82"));            // truncated
    CHECK_FALSE(isValidUtf8("\xC3\x28"));            // bad continuation
    CHECK_FALSE(isValidUtf8("\xC0\xAE"));            // overlong "."
    CHECK_FALSE(isValidUtf8("\xC1\xBF"));            // overlong
    CHECK_FALSE(isValidUtf8("\xE0\x80\xAE"));        // overlong "."
    CHECK_FALSE(isValidUtf8("\xF0\x80\x80\xAE"));    // overlong "."
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));        // surrogate U+D800
    CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80"));    // above U+10FFFF
    CHECK_FALSE(isValidUtf8("\xF5\x80\x80\x80"));
    CHECK_FALSE(isValidUtf8("\xFF"));
}

TEST_CASE("toValidUtf8 replaces bad bytes with U+FFFD") {
    CHECK_EQ(fourfold::toValidUtf8("ok"), "ok");
    CHECK_EQ(fourfold::toValidUtf8("a\xFF" "b"), "a\xEF\xBF\xBD" "b");
    CHECK_EQ(fourfold::toValidUtf8("\xC3"), "\xEF\xBF\xBD");
}

TEST_CASE("toNfc composes, and leaves composed text alone") {
    CHECK_EQ(fourfold::toNfc("cafe\xCC\x81"), "caf\xC3\xA9");     // e + combining acute → é
    CHECK_EQ(fourfold::toNfc("caf\xC3\xA9"), "caf\xC3\xA9");
    CHECK_EQ(fourfold::toNfc("plain"), "plain");
    CHECK_EQ(fourfold::toNfc("\xE2\x84\xAB"), "\xC3\x85");        // ANGSTROM SIGN → Å
    CHECK_EQ(fourfold::toNfc("a\xFF"), "a\xEF\xBF\xBD");           // made well formed first
}

TEST_CASE("utf8Length counts code points") {
    CHECK_EQ(fourfold::utf8Length(""), 0u);
    CHECK_EQ(fourfold::utf8Length("caf\xC3\xA9"), 4u);
    CHECK_EQ(fourfold::utf8Length("\xF0\x9F\x98\x80!"), 2u);
}

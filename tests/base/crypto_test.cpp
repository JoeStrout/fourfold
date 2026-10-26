#include "base/crypto.h"

#include "doctest.h"

using namespace fourfold;

TEST_CASE("hashBytes: BLAKE2b-256 test vectors") {
    CHECK_EQ(toHex(hashBytes("")), "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
    CHECK_EQ(toHex(hashBytes("abc")), "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
}

TEST_CASE("keyedHash: depends on the key and the message") {
    std::string a = keyedHash("key one", "message");
    CHECK_EQ(a.size(), kHashSize);
    CHECK_EQ(a, keyedHash("key one", "message"));
    CHECK_NE(a, keyedHash("key two", "message"));
    CHECK_NE(a, keyedHash("key one", "messagf"));
    CHECK_NE(a, hashBytes("message"));
}

TEST_CASE("constantTimeEqual") {
    CHECK(constantTimeEqual("", ""));
    CHECK(constantTimeEqual("abc", "abc"));
    CHECK_FALSE(constantTimeEqual("abc", "abd"));
    CHECK_FALSE(constantTimeEqual("abc", "ab"));
}

TEST_CASE("base64url round trips, and refuses non-canonical text") {
    for (std::string s : {std::string(), std::string("f"), std::string("fo"), std::string("foo"), std::string("foob"), std::string("fooba"), std::string("foobar"), std::string("\xFF\xFE\0\x01", 4)}) {
        auto back = fromBase64Url(toBase64Url(s));
        REQUIRE(back);
        CHECK_EQ(*back, s);
    }
    CHECK_EQ(toBase64Url("\xFB\xFF"), "-_8");
    CHECK_EQ(toBase64Url("foobar"), "Zm9vYmFy");
    CHECK_FALSE(fromBase64Url("Zm9vYmFy="));   // padding
    CHECK_FALSE(fromBase64Url("Zm+v"));        // standard alphabet
    CHECK_FALSE(fromBase64Url("Z"));           // impossible length
    CHECK_FALSE(fromBase64Url("Zh"));          // unused bits set ("Zg" is canonical)
    CHECK(fromBase64Url("Zg"));
}

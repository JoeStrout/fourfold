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

TEST_CASE("sha256: test vectors") {
    CHECK_EQ(toHex(sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(toHex(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK_EQ(toHex(sha256(std::string(1000, 'a'))),
             "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // 55 and 56 bytes: the padding just fits, and just doesn't.
    CHECK_EQ(toHex(sha256(std::string(55, 'a'))),
             "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK_EQ(toHex(sha256(std::string(56, 'a'))),
             "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

TEST_CASE("leadingZeroBits") {
    CHECK_EQ(leadingZeroBits(std::string("\x80", 1)), 0);
    CHECK_EQ(leadingZeroBits(std::string("\x01", 1)), 7);
    CHECK_EQ(leadingZeroBits(std::string("\x00\x00\x10", 3)), 19);
    CHECK_EQ(leadingZeroBits(std::string("\x00\x00", 2)), 16);
}

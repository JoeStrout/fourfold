#include "partition/paths.h"

#include "doctest.h"

using namespace fourfold;

TEST_CASE("RelPath::parse accepts ordinary paths") {
    for (const char* p : {"about.html", "pages/about.html", "pages/members/photos/beach.jpg",
                          "caf\xC3\xA9.md", "a b/c-d_e.f", "x..y", "trailing.", "CASE/Kept.HTML"}) {
        CAPTURE(p);
        auto r = RelPath::parse(p);
        REQUIRE(r.ok());
        CHECK_EQ(r.value().str(), p);
    }
}

TEST_CASE("RelPath::parse rejects anything that could escape or confuse") {
    std::string longSegment(RelPath::kMaxSegmentBytes + 1, 'a');
    std::string longPath;
    while (longPath.size() <= RelPath::kMaxPathBytes) longPath += "abcdefgh/";
    longPath += "x";
    for (std::string p : {std::string(""), std::string("/etc/passwd"), std::string(".."),
                          std::string("../x"), std::string("a/../b"), std::string("a/./b"),
                          std::string("a//b"), std::string("a/"), std::string(".hidden"),
                          std::string("data/.fourfold/x"), std::string("a\\b"), std::string("..\\x"),
                          std::string("a\nb"), std::string("a\x7F"), std::string("a\0b", 3),
                          std::string("bad\xC0\xAE"), longSegment, longPath}) {
        CAPTURE(p);
        auto r = RelPath::parse(p);
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK(r.error().code == ErrorCode::Invalid);
    }
}

TEST_CASE("RelPath helpers") {
    RelPath p = RelPath::parse("pages/docs/intro.md").value();
    CHECK_EQ(p.name(), "intro.md");
    CHECK_EQ(p.extension(), ".md");
    CHECK_EQ(p.parent().str(), "pages/docs");
    CHECK_EQ(p.parent().parent().parent().str(), "");
    CHECK(p.parent().parent().parent().isRoot());
    auto segs = p.segments();
    REQUIRE_EQ(segs.size(), 3u);
    CHECK_EQ(segs[0], "pages");
    CHECK_EQ(segs[2], "intro.md");

    CHECK_EQ(RelPath::parse("archive.tar.gz").value().extension(), ".gz");
    CHECK_EQ(RelPath::parse("Makefile").value().extension(), "");

    CHECK_EQ(RelPath().child("pages").value().str(), "pages");
    CHECK_EQ(p.parent().child("next.md").value().str(), "pages/docs/next.md");
    CHECK_FALSE(p.child("..").ok());
    CHECK_FALSE(p.child("a/b").ok());

    RelPath pages = RelPath::parse("pages").value();
    CHECK(p.isWithin(pages));
    CHECK(pages.isWithin(pages));
    CHECK(p.isWithin(RelPath()));
    CHECK_FALSE(RelPath::parse("pagesx/a").value().isWithin(pages));
    CHECK_FALSE(pages.isWithin(p));
}

TEST_CASE("canonicalUrlPath: canonical forms") {
    struct Case {
        const char* target;
        const char* path;
        bool trailingSlash;
    };
    for (const Case& c : {
             Case{"/", "/", false},
             Case{"/about", "/about", false},
             Case{"/about?x=1&y=2", "/about", false},
             Case{"/about#top", "/about", false},
             Case{"/docs/", "/docs", true},
             Case{"//admin", "/admin", false},
             Case{"/a//b///c", "/a/b/c", false},
             Case{"/Admin", "/Admin", false},                       // case is kept
             Case{"/caf%C3%A9", "/caf\xC3\xA9", false},             // UTF-8, decoded
             Case{"/caf\xC3\xA9", "/caf\xC3\xA9", false},           // raw UTF-8
             Case{"/a%20b", "/a b", false},
             Case{"/a+b", "/a+b", false},                           // '+' is literal in a path
             Case{"/%61dmin", "/admin", false},
             Case{"/.well-known/x", "/.well-known/x", false},       // dot-names are fine in URLs
             Case{"/a%3Fb", "/a?b", false},
         }) {
        CAPTURE(c.target);
        auto r = canonicalUrlPath(c.target);
        REQUIRE(r.ok());
        CHECK_EQ(r.value().path, c.path);
        CHECK_EQ(r.value().trailingSlash, c.trailingSlash);
    }
}

TEST_CASE("canonicalUrlPath: rejects traversal and encoding tricks") {
    for (std::string target : {
             std::string(""), std::string("about"), std::string("*"),
             std::string("http://example.com/about"),
             std::string("/.."), std::string("/../etc/passwd"), std::string("/a/../b"),
             std::string("/a/./b"), std::string("/."), std::string("/a/.."),
             std::string("/%2e%2e/x"), std::string("/%2E%2E"), std::string("/%2e"), std::string("/.%2e"),
             std::string("/%c0%ae%c0%ae/x"),                      // overlong ".."
             std::string("/a%2Fb"), std::string("/a%2fb"),        // encoded '/'
             std::string("/a%5Cb"), std::string("/a\\b"), std::string("/..\\x"),
             std::string("/a%00b"), std::string("/a%0Ab"), std::string("/a%7F"),
             std::string("/%"), std::string("/a%2"), std::string("/a%zz"), std::string("/a%g0"),
             std::string("/\xFF"), std::string("/%FF"), std::string("/%ED%A0%80"),
             std::string("/") + std::string(5000, 'a'),
         }) {
        CAPTURE(target);
        auto r = canonicalUrlPath(target);
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK(r.error().code == ErrorCode::Invalid);
    }
}

TEST_CASE("encodeUrlPath encodes what a path can't hold, and round-trips") {
    CHECK_EQ(encodeUrlPath("/docs/intro"), "/docs/intro");
    CHECK_EQ(encodeUrlPath("/a b"), "/a%20b");
    CHECK_EQ(encodeUrlPath("/what?#%"), "/what%3F%23%25");
    CHECK_EQ(encodeUrlPath("/caf\xC3\xA9"), "/caf%C3%A9");
    CHECK_EQ(encodeUrlPath("/a\\b\"<>"), "/a%5Cb%22%3C%3E");
    for (const char* p : {"/", "/docs", "/a b/c?d#e%f", "/caf\xC3\xA9/x"}) {
        CAPTURE(p);
        auto back = canonicalUrlPath(encodeUrlPath(p));
        REQUIRE(back.ok());
        CHECK_EQ(back.value().path, p);
    }
}

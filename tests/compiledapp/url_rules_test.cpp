// URL rules' patterns and targets (M11.2).
#include "compiledapp/url_rules.h"

#include "doctest.h"

using namespace fourfold;

namespace {

UrlPattern pattern(const std::string& text) {
    std::string why;
    auto p = UrlPattern::parse(text, why);
    REQUIRE_MESSAGE(p, std::string(text + ": " + why));
    return *p;
}

std::string patternError(const std::string& text) {
    std::string why;
    auto p = UrlPattern::parse(text, why);
    CHECK_FALSE_MESSAGE(p, text);
    return why;
}

UrlPath path(const std::string& target) { return canonicalUrlPath(target).value(); }

// The values a pattern matches a path with, as "name=value;...", or "no".
std::string matched(const std::string& p, const std::string& target) {
    UrlValues values;
    if (!pattern(p).match(path(target), values)) return "no";
    std::string out;
    for (const auto& [name, value] : values) out += name + "=" + value + ";";
    return out;
}

std::string filled(const std::string& p, const std::string& t, const std::string& target) {
    UrlPattern up = pattern(p);
    std::string why;
    auto ut = UrlTarget::parse(t, up, why);
    REQUIRE_MESSAGE(ut, std::string(t + ": " + why));
    UrlValues values;
    REQUIRE(up.match(path(target), values));
    return ut->fill(values);
}

std::string targetError(const std::string& p, const std::string& t) {
    std::string why;
    auto ut = UrlTarget::parse(t, pattern(p), why);
    CHECK_FALSE_MESSAGE(ut, t);
    return why;
}

}  // namespace

TEST_CASE("UrlPattern: placeholders match whole segments") {
    CHECK_EQ(matched("/tryit/{code}", "/tryit/AbCd1234"), "code=AbCd1234;");
    CHECK_EQ(matched("/tryit/{code}", "/tryit/AbCd1234/"), "no");   // the slash must match
    CHECK_EQ(matched("/tryit/{code}", "/tryit"), "no");
    CHECK_EQ(matched("/tryit/{code}", "/tryit/a/b"), "no");
    CHECK_EQ(matched("/tryit/{code}", "/TryIt/a"), "no");           // no case folding
    CHECK_EQ(matched("/a/{x}/c/{y}", "/a/1/c/2"), "x=1;y=2;");
    CHECK_EQ(matched("/a/{x}/c/{y}", "/a/1/d/2"), "no");
    CHECK_EQ(matched("/", "/"), "");
    CHECK_EQ(matched("/", "/x"), "no");
    CHECK_EQ(matched("/old/", "/old/"), "");
    CHECK_EQ(matched("/old/", "/old"), "no");
    // Values are decoded.
    CHECK_EQ(matched("/u/{name}", "/u/Ada%20L%3F"), "name=Ada L?;");
    // Literals are written decoded.
    CHECK_EQ(matched("/my page/{x}", "/my%20page/1"), "x=1;");
}

TEST_CASE("UrlPattern: {name*} matches the rest of the path") {
    CHECK_EQ(matched("/wiki/{rest*}", "/wiki/Main_Page"), "rest=Main_Page;");
    CHECK_EQ(matched("/wiki/{rest*}", "/wiki/a/b/c"), "rest=a/b/c;");
    CHECK_EQ(matched("/wiki/{rest*}", "/wiki/a/b/"), "rest=a/b/;");
    CHECK_EQ(matched("/wiki/{rest*}", "/wiki/"), "rest=;");
    CHECK_EQ(matched("/wiki/{rest*}", "/wiki"), "rest=;");
    CHECK_EQ(matched("/wiki/{rest*}", "/wikipedia"), "no");
    CHECK_EQ(matched("/{all*}", "/x/y"), "all=x/y;");
    CHECK_EQ(matched("/{all*}", "/"), "all=;");
}

TEST_CASE("UrlPattern: what isn't a pattern") {
    CHECK(patternError("tryit") .find("starting with /") != std::string::npos);
    CHECK(patternError("/a?b=1").find("query") != std::string::npos);
    CHECK(patternError("/a//b").find("empty segment") != std::string::npos);
    CHECK(patternError("/a/x{code}").find("whole segment") != std::string::npos);
    CHECK(patternError("/a/{code}.html").find("whole segment") != std::string::npos);
    CHECK(patternError("/a/{1x}").find("placeholder name") != std::string::npos);
    CHECK(patternError("/a/{}").find("placeholder name") != std::string::npos);
    CHECK(patternError("/{x}/{x}").find("twice") != std::string::npos);
    CHECK(patternError("/{rest*}/a").find("must come last") != std::string::npos);
    CHECK(patternError("/a/{rest*}/").find("trailing slash") != std::string::npos);
    CHECK(patternError("/my%20page").find("decoded") != std::string::npos);
    CHECK(patternError("/a/../b").find("isn't a valid") != std::string::npos);
    CHECK(patternError("/__dev/x").find("reserved") != std::string::npos);
    CHECK(patternError("/live/{x}").find("reserved") != std::string::npos);
}

TEST_CASE("UrlPattern: coveredBy finds rules an earlier one shadows") {
    CHECK(pattern("/tryit/abc").coveredBy(pattern("/tryit/{code}")));
    CHECK_FALSE(pattern("/tryit/{code}").coveredBy(pattern("/tryit/abc")));
    CHECK(pattern("/a/b/c").coveredBy(pattern("/a/{rest*}")));
    CHECK(pattern("/a/{x}/c").coveredBy(pattern("/a/{rest*}")));
    CHECK(pattern("/a/{more*}").coveredBy(pattern("/a/{rest*}")));
    CHECK(pattern("/a").coveredBy(pattern("/a/{rest*}")));
    CHECK_FALSE(pattern("/a/{rest*}").coveredBy(pattern("/a/{x}")));
    CHECK_FALSE(pattern("/a/b/").coveredBy(pattern("/a/{x}")));   // trailing slash differs
    CHECK_FALSE(pattern("/b/c").coveredBy(pattern("/a/{rest*}")));
    CHECK(pattern("/x").coveredBy(pattern("/{all*}")));
}

TEST_CASE("UrlTarget: values are encoded for where they land") {
    CHECK_EQ(filled("/tryit/{code}", "/tryit/?s={code}", "/tryit/AbCd1234"), "/tryit/?s=AbCd1234");
    CHECK_EQ(filled("/wiki/{rest*}", "https://wiki.miniscript.org/wiki/{rest}", "/wiki/Main_Page"),
             "https://wiki.miniscript.org/wiki/Main_Page");
    CHECK_EQ(filled("/wiki/{rest*}", "https://wiki.miniscript.org/wiki/{rest}", "/wiki/a/b/"),
             "https://wiki.miniscript.org/wiki/a/b/");
    // A decoded '?', '#', '&' or space can't escape the part it's in.
    CHECK_EQ(filled("/u/{name}", "/people/{name}", "/u/a%3Fb%23c%20d"), "/people/a%3Fb%23c%20d");
    CHECK_EQ(filled("/u/{name}", "/people?n={name}", "/u/a%26b%3Dc%2Bd%20e"), "/people?n=a%26b%3Dc%2Bd%20e");
    CHECK_EQ(filled("/u/{name}", "/p#{name}", "/u/x%20y"), "/p#x%20y");
    // An empty value can't make "//", which a browser would read as another host.
    CHECK_EQ(filled("/wiki/{rest*}", "/{rest}/x", "/wiki/"), "/x");
    CHECK_EQ(filled("/wiki/{rest*}", "/new/{rest}", "/wiki"), "/new/");
    CHECK_EQ(filled("/wiki/{rest*}", "https://h.example/{rest}/x", "/wiki"), "https://h.example/x");
    // Non-ASCII values are encoded too.
    CHECK_EQ(filled("/u/{name}", "/p/{name}", "/u/caf%C3%A9"), "/p/caf%C3%A9");
}

TEST_CASE("UrlTarget: what isn't a target") {
    CHECK(targetError("/a/{x}", "{x}/b").find("starting with /") != std::string::npos);
    CHECK(targetError("/a/{x}", "//{x}").find("starting with /") != std::string::npos);
    CHECK(targetError("/a/{x}", "https://{x}.example.org/").find("choose the host") != std::string::npos);
    CHECK(targetError("/a/{x}", "https:///path").find("no host") != std::string::npos);
    CHECK(targetError("/a/{x}", "https://user@evil/").find("isn't a host name") != std::string::npos);
    CHECK(targetError("/a/{x}", "/b/{y}").find("isn't a placeholder") != std::string::npos);
    CHECK(targetError("/a/{x*}", "/b/{x*}").find("write {x}") != std::string::npos);
    CHECK(targetError("/a/{x}", "/b c").find("space") != std::string::npos);
    CHECK(targetError("/a/{x}", "/b/{x").find("no }") != std::string::npos);
    CHECK(targetError("/a/{x}", "javascript:alert(1)").find("starting with /") != std::string::npos);
}

TEST_CASE("UrlTarget: fixedPath, for checking a rewrite's route") {
    std::string why;
    UrlPattern p = pattern("/tryit/{code}");
    auto t = UrlTarget::parse("/tryit/?s={code}", p, why);
    REQUIRE(t);
    REQUIRE(t->fixedPath());
    CHECK_EQ(t->fixedPath()->path, "/tryit");
    CHECK(t->fixedPath()->trailingSlash);
    CHECK_FALSE(t->hasPathPlaceholders());
    auto u = UrlTarget::parse("/x/{code}", p, why);
    REQUIRE(u);
    CHECK(u->hasPathPlaceholders());
    CHECK_FALSE(u->fixedPath());
    auto v = UrlTarget::parse("https://example.org/x", p, why);
    REQUIRE(v);
    CHECK_FALSE(v->fixedPath());
}

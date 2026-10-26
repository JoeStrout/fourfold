// The escaping rules, table by table: each context, with hostile values
// (overview §4, "Where fields are allowed"). Expected outputs are exact, so
// any change to escaping shows up here.

#include "doctest.h"
#include "helpers.h"

using namespace fourfold;
using namespace fourfold::test;

namespace {

struct Case {
    const char* value;
    const char* expected;
};

void checkAll(const char* templ, const Case* cases, size_t count) {
    for (size_t k = 0; k < count; k++) {
        INFO("template: ", templ, "  value: ", cases[k].value);
        CHECK_EQ(renderFragment(templ, oneString("x", cases[k].value)), cases[k].expected);
    }
}

template <size_t N>
void checkAll(const char* templ, const Case (&cases)[N]) {
    checkAll(templ, cases, N);
}

}  // namespace

TEST_CASE("escape: text") {
    const Case cases[] = {
        {"plain", "<p>plain</p>"},
        {"<script>alert(1)</script>", "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>"},
        {"a & b", "<p>a &amp; b</p>"},
        {"\"quoted\" 'single'", "<p>&quot;quoted&quot; &#39;single&#39;</p>"},
        {"<!--", "<p>&lt;!--</p>"},
        {"</p><img src=x onerror=alert(1)>", "<p>&lt;/p&gt;&lt;img src=x onerror=alert(1)&gt;</p>"},
        {"&lt;already&gt;", "<p>&amp;lt;already&amp;gt;</p>"},
        {"{{y}}", "<p>{{y}}</p>"},   // values are never parsed as templates
        {"caf\xC3\xA9 \xE2\x80\xA8", "<p>caf\xC3\xA9 \xE2\x80\xA8</p>"},
    };
    checkAll("<p>{{x}}</p>", cases);
}

TEST_CASE("escape: RCDATA (<title>, <textarea>)") {
    const Case cases[] = {
        {"</title><script>alert(1)</script>", "<title>&lt;/title&gt;&lt;script&gt;alert(1)&lt;/script&gt;</title>"},
        {"a&b", "<title>a&amp;b</title>"},
    };
    checkAll("<title>{{x}}</title>", cases);
    CHECK_EQ(renderFragment("<textarea name=m>{{x}}</textarea>", oneString("x", "</textarea><b>")),
             "<textarea name=m>&lt;/textarea&gt;&lt;b&gt;</textarea>");
}

TEST_CASE("escape: quoted attributes") {
    const Case dq[] = {
        {"tip", "<a title=\"tip\">"},
        {"\" onmouseover=\"alert(1)", "<a title=\"&quot; onmouseover=&quot;alert(1)\">"},
        {"' onmouseover='alert(1)", "<a title=\"&#39; onmouseover=&#39;alert(1)\">"},
        {"><script>alert(1)</script>", "<a title=\"&gt;&lt;script&gt;alert(1)&lt;/script&gt;\">"},
        {"`backtick`", "<a title=\"`backtick`\">"},
        {"&amp;", "<a title=\"&amp;amp;\">"},
    };
    checkAll("<a title=\"{{x}}\">", dq);
    const Case sq[] = {
        {"' onmouseover='alert(1)", "<a title='&#39; onmouseover=&#39;alert(1)'>"},
    };
    checkAll("<a title='{{x}}'>", sq);
    // A field that's only part of the value.
    const Case part[] = {
        {"big\" onclick=\"x", "<b class=\"note big&quot; onclick=&quot;x\">"},
    };
    checkAll("<b class=\"note {{x}}\">", part);
}

TEST_CASE("escape: a null or missing value leaves the attribute out") {
    CHECK_EQ(renderFragment("<b class=\"{{x}}\">", one("x", FieldValue::null())), "<b>");
    CHECK_EQ(renderFragment("<b id=y class=\"{{x}}\" lang=en>"), "<b id=y lang=en>");
    CHECK_EQ(renderFragment("<b class=\"{{x}}\">", oneString("x", "")), "<b class=\"\">");
    // Only when the field is the whole value.
    CHECK_EQ(renderFragment("<b class=\"a {{x}}\">", one("x", FieldValue::null())), "<b class=\"a \">");
}

TEST_CASE("escape: boolean attributes") {
    const char* t = "<input type=checkbox checked=\"{{x}}\">";
    CHECK_EQ(renderFragment(t, one("x", FieldValue::number("1"))), "<input type=checkbox checked>");
    CHECK_EQ(renderFragment(t, one("x", FieldValue::number("0"))), "<input type=checkbox>");
    CHECK_EQ(renderFragment(t, one("x", FieldValue::null())), "<input type=checkbox>");
    CHECK_EQ(renderFragment(t), "<input type=checkbox>");
    CHECK_EQ(renderFragment(t, oneString("x", "yes")), "<input type=checkbox checked>");
    CHECK_EQ(renderFragment(t, oneString("x", "")), "<input type=checkbox>");
    CHECK_EQ(renderFragment("<option selected='{{x}}' value=a>", one("x", FieldValue::number("2"))),
             "<option selected value=a>");
}

TEST_CASE("escape: URL attributes, at the start") {
    const Case cases[] = {
        {"https://example.com/a?b=c#d", "<a href=\"https://example.com/a?b=c#d\">"},
        {"http://example.com", "<a href=\"http://example.com\">"},
        {"mailto:me@example.com", "<a href=\"mailto:me@example.com\">"},
        {"/local/path", "<a href=\"/local/path\">"},
        {"relative/path?x=1", "<a href=\"relative/path?x=1\">"},
        {"#anchor", "<a href=\"#anchor\">"},
        {"?q=1", "<a href=\"?q=1\">"},
        {"//other.example/x", "<a href=\"//other.example/x\">"},   // http(s) elsewhere is allowed anyway
        {"/a b\"c<d>", "<a href=\"/a%20b%22c%3Cd%3E\">"},
        {"/caf\xC3\xA9", "<a href=\"/caf%C3%A9\">"},
        {"/50%25", "<a href=\"/50%25\">"},
        {"/100%", "<a href=\"/100%25\">"},
        {"/it's", "<a href=\"/it&#39;s\">"},
        {"/a&b", "<a href=\"/a&amp;b\">"},
        // Refused schemes.
        {"javascript:alert(1)", "<a href=\"about:invalid#fourfold\">"},
        {"JavaScript:alert(1)", "<a href=\"about:invalid#fourfold\">"},
        {"  javascript:alert(1)", "<a href=\"about:invalid#fourfold\">"},
        {"\x01javascript:alert(1)", "<a href=\"about:invalid#fourfold\">"},
        {"java\tscript:alert(1)", "<a href=\"about:invalid#fourfold\">"},
        {"java\nscript:alert(1)", "<a href=\"about:invalid#fourfold\">"},
        {"java\rscript:alert(1)", "<a href=\"about:invalid#fourfold\">"},
        {"vbscript:msgbox(1)", "<a href=\"about:invalid#fourfold\">"},
        {"data:text/html,<script>alert(1)</script>", "<a href=\"about:invalid#fourfold\">"},
        {"file:///etc/passwd", "<a href=\"about:invalid#fourfold\">"},
        {"https\x7F://x", "<a href=\"about:invalid#fourfold\">"},
        // Entities in a value are text, not entities: "&#106;" stays literal.
        {"&#106;avascript:alert(1)", "<a href=\"&amp;#106;avascript:alert(1)\">"},
    };
    checkAll("<a href=\"{{x}}\">", cases);
}

TEST_CASE("escape: other URL attributes use the same rule") {
    const char* templates[] = {
        "<img src=\"{{x}}\">",          "<form action=\"{{x}}\">",     "<button formaction=\"{{x}}\">",
        "<video poster=\"{{x}}\">",     "<blockquote cite=\"{{x}}\">", "<object data=\"{{x}}\">",
        "<a data-href=\"{{x}}\">",      "<div hx-get=\"{{x}}\">",      "<div hx-post=\"{{x}}\">",
        "<div hx-push-url=\"{{x}}\">",  "<div data-hx-get=\"{{x}}\">", "<div sse-connect=\"{{x}}\">",
        "<a xlink:href=\"{{x}}\">",     "<x myurl=\"{{x}}\">",         "<x thumbsrc=\"{{x}}\">",
        "<x my-uri=\"{{x}}\">",
    };
    for (const char* t : templates) {
        INFO(t);
        std::string out = renderFragment(t, oneString("x", "javascript:alert(1)"));
        CHECK(out.find("about:invalid#fourfold") != std::string::npos);
        CHECK(out.find("javascript") == std::string::npos);
    }
}

TEST_CASE("escape: a field later in a URL is percent-encoded") {
    const Case query[] = {
        {"2", "<a href=\"/more?page=2\">"},
        {"a b&c=d", "<a href=\"/more?page=a%20b%26c%3Dd\">"},
        {"\"><script>", "<a href=\"/more?page=%22%3E%3Cscript%3E\">"},
        {"javascript:alert(1)", "<a href=\"/more?page=javascript%3Aalert%281%29\">"},
    };
    checkAll("<a href=\"/more?page={{x}}\">", query);
    const Case path[] = {
        {"42", "<a href=\"/users/42\">"},
        {"../admin", "<a href=\"/users/..%2Fadmin\">"},
    };
    checkAll("<a href=\"/users/{{x}}\">", path);
    // A literal prefix means the value can't start the URL, so it can't
    // choose the scheme: "java" + "script:..." stays harmless.
    const Case prefix[] = {
        {"script:alert(1)", "<a href=\"javascript%3Aalert%281%29\">"},
    };
    checkAll("<a href=\"java{{x}}\">", prefix);
    // Leading whitespace still counts as the start.
    CHECK_EQ(renderFragment("<a href=\" {{x}}\">", oneString("x", "javascript:alert(1)")),
             "<a href=\" about:invalid#fourfold\">");
    // Two fields: the first is the start, the second a part.
    CHECK_EQ(renderFragment("<a href=\"{{x}}{{y}}\">",
                            MapFields().set("x", FieldValue::string("/a/")).set("y", FieldValue::string("b/c"))),
             "<a href=\"/a/b%2Fc\">");
}

TEST_CASE("escape: an unsafe URL is reported") {
    ParsedTemplate p = parseFragment("<a href=\"{{x}}\">");
    REQUIRE(p.ok());
    RenderResult r = render(p.compiled, oneString("x", "javascript:alert(1)"));
    REQUIRE(r.ok());
    REQUIRE_EQ(r.warnings.size(), 1);
    CHECK(r.warnings[0].message.find("isn't a safe URL") != std::string::npos);
}

TEST_CASE("escape: <script> values are JSON") {
    const Case cases[] = {
        {"hi", "<script>const c = \"hi\";</script>"},
        {"</script><script>alert(1)</script>",
         "<script>const c = \"\\u003c\\u002fscript\\u003e\\u003cscript\\u003ealert(1)\\u003c\\u002fscript\\u003e\";</script>"},
        {"<!--", "<script>const c = \"\\u003c!--\";</script>"},
        {"\"; alert(1); \"", "<script>const c = \"\\\"; alert(1); \\\"\";</script>"},
        {"'; alert(1); '", "<script>const c = \"\\u0027; alert(1); \\u0027\";</script>"},
        {"`${alert(1)}`", "<script>const c = \"\\u0060${alert(1)}\\u0060\";</script>"},
        {"a/b", "<script>const c = \"a\\u002fb\";</script>"},
        {"line\xE2\x80\xA8sep", "<script>const c = \"line\\u2028sep\";</script>"},
        {"a\nb\\c", "<script>const c = \"a\\nb\\\\c\";</script>"},
    };
    checkAll("<script>const c = {{x}};</script>", cases);

    // Maps, lists, numbers and null.
    Datum::MapItems m;
    m["list"] = Datum::list({Datum::number(1), Datum::string("<b>"), Datum()});
    m["n"] = Datum::number(2.5);
    CHECK_EQ(renderFragment("<script>const c = {{x}};</script>", one("x", FieldValue::datum(Datum::map(m)))),
             "<script>const c = {\"list\":[1,\"\\u003cb\\u003e\",null],\"n\":2.5};</script>");
    CHECK_EQ(renderFragment("<script>f({{x}})</script>", one("x", FieldValue::number("3"))), "<script>f(3)</script>");
    CHECK_EQ(renderFragment("<script>f({{x}})</script>"), "<script>f(null)</script>");
    CHECK_EQ(renderFragment("<script type=\"application/json\">{\"a\": {{x}}}</script>", oneString("x", "v")),
             "<script type=\"application/json\">{\"a\": \"v\"}</script>");
}

TEST_CASE("escape: the nonce goes on every <script>") {
    RenderOptions o;
    o.nonce = "abc123";
    CHECK_EQ(renderFragment("<script src=\"/x.js\"></script><SCRIPT>1</SCRIPT>", EmptyFields(), o),
             "<script nonce=\"abc123\" src=\"/x.js\"></script><SCRIPT nonce=\"abc123\">1</SCRIPT>");
    // Not on things that only look like scripts.
    CHECK_EQ(renderFragment("<p>&lt;script&gt;</p><scripts></scripts>", EmptyFields(), o),
             "<p>&lt;script&gt;</p><scripts></scripts>");
    // None without one.
    CHECK_EQ(renderFragment("<script>1</script>"), "<script>1</script>");
}

TEST_CASE("escape: rendered HTML goes only in text") {
    FieldValue h = FieldValue::markup(unsafeHtml("<b>bold</b>"));
    CHECK_EQ(renderFragment("<p>{{x}}</p>", one("x", h)), "<p><b>bold</b></p>");
    CHECK(renderFragment("<p title=\"{{x}}\">", one("x", h)).find("ERROR: {{x}} is rendered HTML") == 0);
    CHECK(renderFragment("<a href=\"{{x}}\">", one("x", h)).find("ERROR:") == 0);
    CHECK(renderFragment("<title>{{x}}</title>", one("x", h)).find("ERROR:") == 0);
    CHECK(renderFragment("<script>f({{x}})</script>", one("x", h)).find("ERROR:") == 0);
    CHECK(renderFragment("<input checked=\"{{x}}\">", one("x", h)).find("ERROR:") == 0);
}

TEST_CASE("escape: maps and lists only in a <script>") {
    FieldValue m = FieldValue::datum(Datum::map({}));
    CHECK(renderFragment("<p>{{x}}</p>", one("x", m)).find("ERROR: {{x}} is a map") == 0);
    CHECK(renderFragment("<p title=\"{{x}}\">", one("x", FieldValue::datum(Datum::list({}))))
              .find("ERROR: {{x}} is a list") == 0);
}

TEST_CASE("escape: dotted paths") {
    Datum::MapItems user;
    user["name"] = Datum::string("<Ann>");
    Datum::MapItems errors;
    MapFields f;
    f.set("user", FieldValue::datum(Datum::map(user)));
    f.set("errors", FieldValue::datum(Datum::map(errors)));
    f.set("none", FieldValue::null());
    f.set("s", FieldValue::string("text"));
    CHECK_EQ(renderFragment("<p>{{user.name}}</p>", f), "<p>&lt;Ann&gt;</p>");
    CHECK_EQ(renderFragment("<p>{{ user.name }}</p>", f), "<p>&lt;Ann&gt;</p>");
    CHECK_EQ(renderFragment("<p>{{errors.who}}</p>", f), "<p></p>");   // absent key: empty
    CHECK_EQ(renderFragment("<p>{{none.x}}</p>", f), "<p></p>");       // through null: empty
    CHECK(renderFragment("<p>{{s.x}}</p>", f).find("ERROR: {{s.x}} s isn't a map") == 0);
    CHECK_EQ(renderFragment("<script>u = {{user}}</script>", f), "<script>u = {\"name\":\"\\u003cAnn\\u003e\"}</script>");
}

TEST_CASE("escape: \\{{ is a literal {{ everywhere") {
    CHECK_EQ(renderFragment("<p>\\{{x}}</p>"), "<p>{{x}}</p>");
    CHECK_EQ(renderFragment("<a title=\"\\{{x}}\">"), "<a title=\"{{x}}\">");
    CHECK_EQ(renderFragment("<!-- \\{{x}} -->"), "<!-- {{x}} -->");
    CHECK_EQ(renderFragment("<style>a{} \\{{x}}</style>"), "<style>a{} {{x}}</style>");
    CHECK_EQ(renderFragment("<script>s = '\\{{x}}'</script>"), "<script>s = '{{x}}'</script>");
}

TEST_CASE("escape: missing and unused fields are reported") {
    ParsedTemplate p = parseFragment("<p>{{a}} {{b}} {{a}}</p>");
    REQUIRE(p.ok());
    MapFields f;
    f.set("b", FieldValue::string("x"));
    f.set("extra", FieldValue::string("y"));
    RenderResult r = render(p.compiled, f);
    REQUIRE(r.ok());
    CHECK_EQ(r.html.str(), "<p> x </p>");
    REQUIRE_EQ(r.warnings.size(), 2);
    CHECK(r.warnings[0].message.find("{{a}} wasn't given a value") == 0);
    CHECK(r.warnings[1].message.find("\"extra\" was given a value") == 0);

    RenderOptions quiet;
    quiet.warnings = false;
    CHECK(render(p.compiled, f, quiet).warnings.empty());
}

TEST_CASE("escape: the helpers") {
    CHECK_EQ(escapeHtml("a&b<c>d\"e'f"), "a&amp;b&lt;c&gt;d&quot;e&#39;f");
    CHECK_EQ(percentEncode("a-z_A.Z~0 /:?#&="), "a-z_A.Z~0%20%2F%3A%3F%23%26%3D");
    CHECK_EQ(urlScheme("HTTPS://x"), "https");
    CHECK_EQ(urlScheme("/a:b"), "");
    CHECK_EQ(urlScheme("a/b:c"), "");
    CHECK_EQ(urlScheme("a?b:c"), "");
    CHECK_EQ(urlScheme("javascript:x"), "javascript");
    CHECK_FALSE(filterUrl("javascript:x"));
    CHECK_EQ(*filterUrl("  https://x/"), "https://x/");
    CHECK_FALSE(filterUrl("https://x/\ny"));
}

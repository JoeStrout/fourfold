#include "template/template.h"

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

// Templates come only from CodeFile, so tests go through a real code tree.
ParsedTemplate parse(const std::string& text) {
    TempDir dir;
    dir.write("code/pages/t.html", text);
    auto app = AppFolder::open(dir.path(), AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto code = app.value().codeTree();
    REQUIRE(code.ok());
    auto file = code.value().read(RelPath::parse("pages/t.html").value());
    REQUIRE(file.ok());
    return parseTemplate(file.value());
}

std::string render(const std::string& text, const TemplateFields& fields = {}) {
    ParsedTemplate p = parse(text);
    REQUIRE(p.ok());
    return renderTemplate(p.compiled, fields);
}

// The first error's message, or "" if it parsed.
std::string firstError(const std::string& text) {
    ParsedTemplate p = parse(text);
    return p.ok() ? "" : p.errors[0].message;
}

}  // namespace

TEST_CASE("template: a page without fields renders unchanged") {
    std::string page = "<!DOCTYPE html>\n<html><head><title>Hi</title><style>p{color:red}</style></head>\n"
                       "<body><p class=\"x\">a < b && c > d</p><!-- note --><script>if (a<b) {}</script></body></html>\n";
    ParsedTemplate p = parse(page);
    REQUIRE(p.ok());
    CHECK_FALSE(p.compiled.hasFields());
    CHECK_EQ(renderTemplate(p.compiled, {}), page);
}

TEST_CASE("template: fields in text are filled and escaped") {
    CHECK_EQ(render("<p>{{name}}</p>", {{"name", "Ada"}}), "<p>Ada</p>");
    CHECK_EQ(render("<p>{{ name }}</p>", {{"name", "Ada"}}), "<p>Ada</p>");
    CHECK_EQ(render("<p>{{msg}}</p>", {{"msg", "<script>alert('x')</script> & \"q\""}}),
             "<p>&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt; &amp; &quot;q&quot;</p>");
    CHECK_EQ(render("{{a}}{{b}}{{a}}", {{"a", "1"}, {"b", "2"}}), "121");
}

TEST_CASE("template: missing and null fields render empty") {
    CHECK_EQ(render("<p>[{{x}}]</p>"), "<p>[]</p>");
    CHECK_EQ(render("<p>[{{x}}]</p>", {{"x", std::nullopt}}), "<p>[]</p>");
}

TEST_CASE("template: field names are listed once, in order") {
    ParsedTemplate p = parse("{{b}} {{a}} {{b}}");
    REQUIRE(p.ok());
    CHECK_EQ(p.compiled.fieldNames(), std::vector<std::string>{"b", "a"});
}

TEST_CASE("template: \\{{ is a literal {{, in any context") {
    CHECK_EQ(render("<p>\\{{name}}</p>"), "<p>{{name}}</p>");
    CHECK_EQ(render("<a title=\"\\{{x}}\">t</a>"), "<a title=\"{{x}}\">t</a>");
    CHECK_EQ(render("<script>let s = '\\{{';</script>"), "<script>let s = '{{';</script>");
    CHECK_EQ(render("<!-- \\{{x}} -->"), "<!-- {{x}} -->");
}

TEST_CASE("template: fields anywhere but text are compile errors") {
    const char* cases[] = {
        "<a href=\"{{url}}\">x</a>",
        "<a href='{{url}}'>x</a>",
        "<a href={{url}}>x</a>",
        "<a {{attr}}>x</a>",
        "<{{tag}}>",
        "</{{tag}}>",
        "x<{{tag}} y>",
        "</p {{x}}>",
        "<p title=\"a > b {{x}}\">",          // the '>' is inside quotes; still in the tag
        "<!-- {{x}} -->",
        "<script>var x = {{x}};</script>",
        "<SCRIPT>var x = {{x}};</SCRIPT>",
        "<script type=\"module\">{{x}}</script>",
        "<style>p { color: {{c}} }</style>",
        "<textarea>{{t}}</textarea>",
        "<title>{{t}}</title>",
        "<noscript>{{x}}</noscript>",
        "<iframe>{{x}}</iframe>",
        "<!DOCTYPE {{x}}>",
        "<?xml {{x}}?>",
        "<script>'</scr' + 'ipt>'; {{x}}</script>",   // not an end tag
    };
    for (const char* c : cases) {
        std::string text = c;
        CAPTURE(text);
        ParsedTemplate p = parse(c);
        CHECK_FALSE(p.ok());
    }
}

TEST_CASE("template: text resumes after tags, comments, and raw-text elements end") {
    CHECK_EQ(render("<a href=\"/x\">{{t}}</a>", {{"t", "go"}}), "<a href=\"/x\">go</a>");
    CHECK_EQ(render("<!-- c -->{{t}}", {{"t", "1"}}), "<!-- c -->1");
    CHECK_EQ(render("<script>x</script >{{t}}", {{"t", "1"}}), "<script>x</script >1");
    CHECK_EQ(render("<style>p{}</STYLE>{{t}}", {{"t", "1"}}), "<style>p{}</STYLE>1");
    CHECK_EQ(render("<p>1 < 2 {{t}}</p>", {{"t", "ok"}}), "<p>1 < 2 ok</p>");
    CHECK_EQ(render("<br/>{{t}}", {{"t", "1"}}), "<br/>1");
}

TEST_CASE("template: malformed fields are errors, with their line") {
    ParsedTemplate p = parse("<p>\n\n{{ two words }}</p>");
    REQUIRE_FALSE(p.ok());
    CHECK_EQ(p.errors[0].line, 3);
    CHECK(firstError("{{}}").find("malformed") != std::string::npos);
    CHECK(firstError("{{1x}}").find("malformed") != std::string::npos);
    CHECK(firstError("{{name").find("malformed") != std::string::npos);
    CHECK(firstError("{{user.name}}").find("dotted") != std::string::npos);
    CHECK(firstError("{{> header}}").find("includes") != std::string::npos);
}

TEST_CASE("template: an error's line counts lines in tags and comments too") {
    ParsedTemplate p = parse("<p\n title='x\n'>\n<!--\n-->\n<b title={{x}}>");
    REQUIRE_FALSE(p.ok());
    CHECK_EQ(p.errors[0].line, 6);
}

TEST_CASE("escapeHtml") {
    CHECK_EQ(escapeHtml("a&b<c>d\"e'f"), "a&amp;b&lt;c&gt;d&quot;e&#39;f");
    CHECK_EQ(escapeHtml("caf\xC3\xA9"), "caf\xC3\xA9");
}

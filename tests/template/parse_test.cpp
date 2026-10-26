// The parser: contexts, every compile error, pages and fragments, includes.

#include "doctest.h"
#include "helpers.h"

using namespace fourfold;
using namespace fourfold::test;

namespace {

// The contexts of a fragment's fields, in order, as "name:context".
std::string contexts(const std::string& text) {
    ParsedTemplate p = parseFragment(text);
    INFO(text);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.errors[0].message));
    std::string out;
    for (const CompiledTemplate::Part& part : p.compiled.parts()) {
        if (part.kind != CompiledTemplate::Part::Kind::Field && part.kind != CompiledTemplate::Part::Kind::Attribute) {
            continue;
        }
        if (!out.empty()) out += " ";
        out += part.field.label() + ":" + fieldContextName(part.field.context);
        if (part.kind == CompiledTemplate::Part::Kind::Attribute) out += "(whole)";
    }
    return out;
}

void refused(const std::string& text, const std::string& expected) {
    INFO(text);
    std::string e = fragmentError(text);
    CHECK_MESSAGE(e.find(expected) != std::string::npos, "got: \"" << e << "\"");
}

const char* const kPage = "<!DOCTYPE html>\n<html><head><title>T</title></head><body>{{x}}</body></html>\n";

}  // namespace

TEST_CASE("parse: contexts") {
    CHECK_EQ(contexts("<p>{{a}}</p>"), "a:text");
    CHECK_EQ(contexts("<title>{{a}}</title><textarea>{{b}}</textarea>"), "a:rcdata b:rcdata");
    CHECK_EQ(contexts("<a title=\"{{a}}\" class='x {{b}}'>"), "a:attribute(whole) b:attribute");
    CHECK_EQ(contexts("<a href=\"{{a}}\" src=\"/x?{{b}}\">"), "a:url(whole) b:url-part");
    CHECK_EQ(contexts("<input checked=\"{{a}}\">"), "a:boolean(whole)");
    CHECK_EQ(contexts("<script>var a = {{a}}, b = [{{b}}];</script>"), "a:script b:script");
    CHECK_EQ(contexts("<p>{{ user.name }}</p>"), "user.name:text");
    // After an element ends, fields are back in text.
    CHECK_EQ(contexts("<script>1</script>{{a}}<style>p{}</style>{{b}}"), "a:text b:text");
    // '>' inside a quoted value doesn't end the tag.
    CHECK_EQ(contexts("<a title=\"a>b\" href=\"{{a}}\">{{b}}"), "a:url(whole) b:text");
    // Comments end where browsers end them.
    CHECK_EQ(contexts("<!-->{{a}}"), "a:text");
    CHECK_EQ(contexts("<!--->{{a}}"), "a:text");
    CHECK_EQ(contexts("<!-- x --!>{{a}}"), "a:text");
    CHECK_EQ(contexts("<!-- -- > still -->{{a}}"), "a:text");
    // Inside <svg>, attributes and text without fields are fine.
    CHECK_EQ(contexts("<svg viewBox=\"0 0 1 1\"><path d=\"M0\"/><title>Icon</title></svg>{{a}}"), "a:text");
    // Raw-text end tags need a delimiter after the name.
    CHECK_EQ(contexts("<textarea></textareax>{{a}}</textarea>"), "a:rcdata");
}

TEST_CASE("parse: fields in the wrong places are compile errors") {
    refused("<a title={{x}}>", "unquoted attribute value");
    refused("<{{x}}>", "can't follow '<'");
    refused("<p>a <{{x}}", "can't follow '<'");
    refused("</{{x}}>", "a field isn't allowed inside </...>");
    refused("<a{{x}}>", "in a tag name");
    refused("<a {{x}}=\"1\">", "attribute name");
    refused("<a {{x}}>", "attribute name");
    refused("<a onclick=\"{{x}}\">", "inline event handlers");
    refused("<b style=\"color: {{x}}\">", "style attribute");
    refused("<style>p { color: {{x}} }</style>", "inside <style>");
    refused("<!-- {{x}} -->", "inside an HTML comment");
    refused("<img srcset=\"{{x}} 2x\">", "srcset");
    refused("<iframe srcdoc=\"{{x}}\"></iframe>", "srcdoc");
    refused("<iframe>{{x}}</iframe>", "inside <iframe>");
    refused("<noscript>{{x}}</noscript>", "inside <noscript>");
    refused("<xmp>{{x}}</xmp>", "inside <xmp>");
    refused("<noembed>{{x}}</noembed>", "inside <noembed>");
    refused("<noframes>{{x}}</noframes>", "inside <noframes>");
    refused("<base href=\"{{x}}\">", "<base>");
    refused("<meta http-equiv=\"refresh\" content=\"0;url={{x}}\">", "http-equiv");
    refused("<meta content=\"{{x}}\" http-equiv=\"refresh\">", "http-equiv");
    refused("<svg><text>{{x}}</text></svg>", "inside <svg> or <math>");
    refused("<svg><a href=\"{{x}}\"></a></svg>", "inside <svg> or <math>");
    refused("<math><mi>{{x}}</mi></math>", "inside <svg> or <math>");
    refused("<div hx-vals='{\"a\": \"{{x}}\"}'>", "hx-vals");
    refused("<div hx-headers='{\"a\": \"{{x}}\"}'>", "hx-headers");
    refused("<div hx-trigger=\"{{x}}\">", "hx-trigger");
    refused("<input checked=\"yes {{x}}\">", "boolean attribute");
    refused("<input checked=\"{{x}}{{y}}\">", "boolean attribute");
    refused("<script type=\"text/template\">{{x}}</script>", "only allowed in a JavaScript or JSON <script>");
    refused("<a title=\"{{> header}}\">", "an include can only go in text");
    refused("<title>{{> header}}</title>", "an include can only go in text");
    refused("<![CDATA[ {{x}} ]]>", "a field isn't allowed inside <!...>");
    refused("<?php {{x}} ?>", "a field isn't allowed inside <!...>");
    refused("</p title=\"{{x}}\">", "in an end tag");
}

TEST_CASE("parse: fields in <script> must be where a value goes") {
    refused("<script>s = \"{{x}}\";</script>", "inside a JavaScript string");
    refused("<script>s = '{{x}}';</script>", "inside a JavaScript string");
    refused("<script>s = `a {{x}}`;</script>", "template literal");
    refused("<script>s = `${ {{x}} }`;</script>", "${...}");
    refused("<script>// {{x}}\n</script>", "comment");
    refused("<script>/* {{x}} */</script>", "comment");
    refused("<script>r = /a{{x}}/;</script>", "regular expression");
    refused("<script>r = /[/]{{x}}/;</script>", "regular expression");
    // After ')' a '/' may start a regular expression: both readings are
    // followed, so this field is refused...
    refused("<script>if (a) /x{{x}}/.test(s);</script>", "regular expression");
    refused("<script>}/ {{x}}</script>", "regular expression");
    // ... and one that looks like code only under the division reading is
    // refused too, since under the other it's in a string.
    refused("<script>if (a) /\"/.test(s); y = \"{{x}}\";</script>", "string");
    // But the regex reading dies at a line end, so this is fine.
    CHECK_EQ(contexts("<script>x = (a) / 2\ny = {{x}};</script>"), "x:script");
    // Ordinary code.
    CHECK_EQ(contexts("<script>x = a / 2; y = {{x}};</script>"), "x:script");
    CHECK_EQ(contexts("<script>x = a[1] / {{x}};</script>"), "x:script");
    CHECK_EQ(contexts("<script>return /re/.test({{x}});</script>"), "x:script");
    CHECK_EQ(contexts("<script>s = \"a\\\"b\"; t = {{x}};</script>"), "x:script");
    CHECK_EQ(contexts("<script>s = `a${b}c`; t = {{x}};</script>"), "x:script");
    CHECK_EQ(contexts("<script>s = `a${ {a: 1}.a }c`; t = {{x}};</script>"), "x:script");
    CHECK_EQ(contexts("<script>/* a */ // b\n t = {{x}};</script>"), "x:script");
    CHECK_EQ(contexts("<script>r = /a\\/b[/]/g; t = {{x}} / 2;</script>"), "x:script");
    CHECK_EQ(contexts("<script>i++ / 2; t = {{x}};</script>"), "x:script");
    // A string or regular expression left open loses the reading.
    refused("<script>s = \"abc\n t = {{x}};</script>", "couldn't be followed");
}

TEST_CASE("parse: <script> content that changes how browsers end it") {
    refused("<script>document.write(\"<!--\")</script>", "can't contain \"<!--\" or \"<script\"");
    refused("<script>s = \"<SCRIPT>\"</script>", "can't contain");
    CHECK_EQ(fragmentError("<script>s = \"<scr\" + \"ipt>\"</script>"), "");
}

TEST_CASE("parse: attributes refused with or without fields") {
    refused("<a onclick=\"go()\">", "inline event handlers like onclick=");
    refused("<svg onload=\"go()\"></svg>", "inline event handlers");
    refused("<button hx-on:click=\"go()\">", "runs inline JavaScript");
    refused("<button hx-on::after-request=\"go()\">", "runs inline JavaScript");
    refused("<button data-hx-on-click=\"go()\">", "runs inline JavaScript");
    refused("<div hx-vars=\"a:1\">", "hx-vars");
    refused("<div hx-vals=\"js:{a: f()}\">", "js: evaluates JavaScript");
    refused("<div hx-headers=' javascript:{}'>", "js: evaluates JavaScript");
    refused("<script nonce=\"x\"></script>", "don't write nonce attributes");
    refused("<meta name=\"htmx-config\" content='{\"allowEval\": true}'>", "htmx's settings are fixed");
    CHECK_EQ(fragmentError("<div hx-vals='{\"a\": 1}'>"), "");
}

TEST_CASE("parse: foreign content") {
    refused("<svg><p>x</p></svg>", "<p> inside <svg> or <math> ends it early");
    refused("<svg><style>a{}</style></svg>", "<style> inside <svg> or <math> isn't supported");
    refused("<svg><script></script></svg>", "<script> inside <svg>");
    refused("<svg><foreignObject></foreignObject></svg>", "<foreignobject> inside");
    refused("<svg><title><b>x</b></title></svg>", "tags inside <svg>'s <title>");
    refused("<svg>", "unclosed <svg>");
    CHECK_EQ(contexts("<svg><svg></svg><![CDATA[ > ]]></svg><p title=\"{{x}}\">"), "x:attribute(whole)");
}

TEST_CASE("parse: unclosed things are errors") {
    refused("<!-- open", "unclosed comment");
    refused("<a href=\"x", "unclosed attribute value");
    refused("<a href=x", "unclosed tag");
    refused("<script>1", "unclosed <script>");
    refused("<textarea>1", "unclosed <textarea>");
    CHECK(pageError("<!DOCTYPE html").find("unclosed <!DOCTYPE>") == 0);
    CHECK(pageError("<!DOCTYPE {{x}}>").find("a field isn't allowed inside <!DOCTYPE>") == 0);
    refused("<plaintext>", "<plaintext> isn't supported");
}

TEST_CASE("parse: malformed fields") {
    refused("{{}}", "malformed field");
    refused("{{a b}}", "malformed field");
    refused("{{a.}}", "malformed field");
    refused("{{.a}}", "malformed field");
    refused("{{a-b}}", "malformed field");
    refused("{{1a}}", "malformed field");
    refused("{{a", "malformed field");
    refused("{{>}}", "malformed include");
    refused("{{> a b}}", "malformed include");
    refused("{{> /a}}", "malformed include");
    refused(std::string("a\0b", 3), "NUL byte");
}

TEST_CASE("parse: line numbers") {
    ParsedTemplate p = parseFragment("<p>\n\n{{ two words }}</p>\n<a\n title={{x}}>");
    REQUIRE_EQ(p.errors.size(), 2);
    CHECK_EQ(p.errors[0].line, 3);
    CHECK_EQ(p.errors[1].line, 5);
    CHECK_EQ(p.errors[0].file, "frags/t.html");
    ParsedTemplate q = parseFragment("<p\n title='x\n'>\n<!--\n-->\n<b title=\"{{x}}\">");
    REQUIRE(q.ok());
    CHECK_EQ(q.compiled.parts()[1].field.line, 6);
}

TEST_CASE("parse: pages must be documents; fragments must not") {
    CHECK_EQ(pageError(kPage), "");
    CHECK_EQ(pageError("\xEF\xBB\xBF<!-- c -->\n  <!doctype html><p>x"), "");
    CHECK_EQ(pageError("<html><body></body></html>"), "");
    CHECK(pageError("<p>hi</p>").find("a page must be a complete document") == 0);
    CHECK(pageError("hello <!DOCTYPE html>").find("a page must be a complete document") == 0);
    CHECK(pageError("{{x}}<!DOCTYPE html>").find("a page must be a complete document") == 0);
    CHECK(pageError("").find("a page must be a complete document") == 0);
    refused("<!DOCTYPE html>", "a fragment can't have a doctype");
    refused("<html>", "a fragment can't have <html>");
    refused("<HEAD>", "a fragment can't have <head>");
    refused("<body class=x>", "a fragment can't have <body>");
    refused("</body>", "a fragment can't have </body>");
}

TEST_CASE("parse: what a template records") {
    ParsedTemplate p = parsePage(
        "<!DOCTYPE html><html><head><script src=a.js></script></head>"
        "<body><form method=\"POST\" action=/x>{{a}}</form><form method=get></form>{{b}} {{a}}</body></html>");
    REQUIRE(p.ok());
    CHECK(p.compiled.hasScript());
    CHECK_EQ(p.compiled.postForms(), 1);
    CHECK_EQ(p.compiled.fieldNames(), std::vector<std::string>{"a", "b"});
    size_t heads = 0, nonces = 0;
    for (const auto& part : p.compiled.parts()) {
        heads += part.kind == CompiledTemplate::Part::Kind::HeadEnd;
        nonces += part.kind == CompiledTemplate::Part::Kind::Nonce;
    }
    CHECK_EQ(heads, 1);
    CHECK_EQ(nonces, 1);
    CHECK_FALSE(parseFragment("<p>x</p>").compiled.hasScript());
}

TEST_CASE("parse: markdown.css goes at the end of <head>") {
    ParsedTemplate p = parsePage("<!DOCTYPE html><html><head><title>t</title></head><body>{{x}}</body></html>");
    REQUIRE(p.ok());
    FieldValue md = FieldValue::markup(unsafeHtml("<p>x</p>"));
    RenderOptions o;
    o.markdownCss = "/markdown.css";
    // Only when the page holds Markdown; unsafeHtml isn't.
    CHECK(render(p.compiled, one("x", md), o).html.str().find("markdown.css") == std::string::npos);

    // Without </head>, before <body>.
    ParsedTemplate q = parsePage("<!DOCTYPE html><title>t</title><body>x</body>");
    REQUIRE(q.ok());
    bool found = false;
    for (const auto& part : q.compiled.parts()) found = found || part.kind == CompiledTemplate::Part::Kind::HeadEnd;
    CHECK(found);
}

TEST_CASE("parse: includes") {
    FragmentMap frags;
    frags["header"] = parseTemplate(codeFile("frags/header.html", "<header>{{title}} {{> nav}}</header>"),
                                    TemplateKind::Fragment).compiled;
    frags["nav"] = parseTemplate(codeFile("frags/nav.html", "<nav><a href=\"{{home}}\">h</a></nav>"),
                                 TemplateKind::Fragment).compiled;
    frags["parts/foot"] = parseTemplate(codeFile("frags/parts/foot.html", "<script>f()</script>"),
                                        TemplateKind::Fragment).compiled;
    CHECK(resolveIncludes(frags).empty());

    ParsedTemplate page = parsePage("<!DOCTYPE html><body>{{> header}}<main>{{body}}</main>{{> parts/foot}}</body>");
    REQUIRE(page.ok());
    CHECK(resolveIncludes(page.compiled, frags).empty());
    CHECK_EQ(page.compiled.fieldNames(), std::vector<std::string>{"body", "title", "home"});
    CHECK(page.compiled.hasScript());

    MapFields f;
    f.set("title", FieldValue::string("<T>"));
    f.set("home", FieldValue::string("javascript:x"));
    f.set("body", FieldValue::string("b"));
    RenderOptions o;
    o.nonce = "n";
    RenderResult r = render(page.compiled, f, o);
    REQUIRE(r.ok());
    CHECK_EQ(r.html.str(),
             "<!DOCTYPE html><body><header>&lt;T&gt; <nav><a href=\"about:invalid#fourfold\">h</a></nav></header>"
             "<main>b</main><script nonce=\"n\">f()</script></body>");
    // A field's file is where it's written, for errors and warnings.
    REQUIRE_FALSE(r.warnings.empty());
    CHECK_EQ(r.warnings[0].file, "frags/nav.html");
}

TEST_CASE("parse: include problems") {
    FragmentMap frags;
    frags["a"] = parseTemplate(codeFile("frags/a.html", "<i>{{> b}}</i>"), TemplateKind::Fragment).compiled;
    frags["b"] = parseTemplate(codeFile("frags/b.html", "<i>{{> a}}</i>"), TemplateKind::Fragment).compiled;
    frags["self"] = parseTemplate(codeFile("frags/self.html", "{{> self}}"), TemplateKind::Fragment).compiled;
    frags["gone"] = parseTemplate(codeFile("frags/gone.html", "\n{{> nothing}}"), TemplateKind::Fragment).compiled;
    std::vector<TemplateProblem> problems = resolveIncludes(frags);
    std::string all;
    for (const TemplateProblem& p : problems) all += p.file + ":" + std::to_string(p.line) + ": " + p.message + "\n";
    CHECK_MESSAGE(all.find("include cycle: a → b → a") != std::string::npos, all);
    CHECK_MESSAGE(all.find("include cycle: self → self") != std::string::npos, all);
    CHECK_MESSAGE(all.find("frags/gone.html:2: there's no code/frags/nothing.html") != std::string::npos, all);

    // A fragment must end where it started, in text, to be included.
    refused("<a href=\"x\"", "unclosed tag");
    refused("<script>", "unclosed <script>");
}

TEST_CASE("parse: Markdown fragments") {
    FragmentMap frags;
    frags["intro"] = CompiledTemplate::fromMarkdown(RelPath::parse("frags/intro.md").value(),
                                                    unsafeHtml("<div class=\"markdown\"><p>Hi</p></div>"));
    ParsedTemplate page = parsePage("<!DOCTYPE html><html><head></head><body>{{> intro}}</body></html>");
    REQUIRE(page.ok());
    CHECK(resolveIncludes(page.compiled, frags).empty());
    CHECK(page.compiled.hasMarkdown());
    RenderOptions o;
    o.markdownCss = "/__fourfold/markdown.css";
    CHECK_EQ(render(page.compiled, EmptyFields(), o).html.str(),
             "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"/__fourfold/markdown.css\">\n</head>"
             "<body><div class=\"markdown\"><p>Hi</p></div></body></html>");
}

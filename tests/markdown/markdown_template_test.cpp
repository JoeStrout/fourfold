// Dynamic Markdown: .md files in code/ as templates (M6.1; overview §4,
// Fields in Markdown).

#include <regex>

#include "../template/helpers.h"
#include "doctest.h"
#include "markdown/markdown.h"

using namespace fourfold;
using namespace fourfold::test;

namespace {

ParsedTemplate parseMd(const std::string& text, TemplateKind kind = TemplateKind::Fragment) {
    return parseMarkdownTemplate(codeFile(kind == TemplateKind::Page ? "pages/t.md" : "frags/t.md", text), kind);
}

CompiledTemplate md(const std::string& text, TemplateKind kind = TemplateKind::Fragment) {
    ParsedTemplate p = parseMd(text, kind);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.errors[0].message));
    return std::move(p.compiled);
}

std::string mdError(const std::string& text) {
    ParsedTemplate p = parseMd(text);
    return p.ok() ? "" : p.errors[0].message;
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

RenderResult renderAs(const CompiledTemplate& t, const FieldSource& f, OutputFormat format) {
    RenderOptions o;
    o.format = format;
    return render(t, f, o);
}

std::string htmlOf(const CompiledTemplate& t, const FieldSource& f) {
    RenderResult r = renderAs(t, f, OutputFormat::Html);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error->message));
    return r.html.str();
}

std::string markdownOf(const CompiledTemplate& t, const FieldSource& f) {
    RenderResult r = renderAs(t, f, OutputFormat::Markdown);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error->message));
    return r.markdown;
}

// The HTML without its wrapper.
std::string inner(const std::string& html) {
    size_t start = html.find('\n') + 1;
    return html.substr(start, html.size() - start - std::string("</div>\n").size());
}

MapFields one(const std::string& name, const std::string& value) {
    MapFields f;
    f.set(name, FieldValue::string(value));
    return f;
}

}  // namespace

TEST_CASE("Markdown templates: fields, in both forms") {
    CompiledTemplate t = md("# Hi, {{name}}\n\nYou have **{{count}}** new {{things.kind}}.\n");
    CHECK(t.isMarkdown());
    CHECK(t.hasMarkdown());
    CHECK_EQ(t.fieldNames(), std::vector<std::string>{"name", "count", "things"});
    MapFields f;
    f.set("name", FieldValue::string("<Ann> & *co*"));
    f.set("count", FieldValue::number("3"));
    f.set("things", FieldValue::datum(Datum::map({{"kind", Datum::string("notes")}})));
    // The heading's id leaves the field out, so it doesn't change with it.
    CHECK_EQ(inner(htmlOf(t, f)), "<h1 id=\"hi-\">Hi, &lt;Ann&gt; &amp; *co*</h1>\n"
                                  "<p>You have <strong>3</strong> new notes.</p>\n");
    CHECK_EQ(markdownOf(t, f), "# Hi, \\<Ann\\> \\& \\*co\\*\n\nYou have **3** new notes.\n");
    // With no values, fields are empty.
    CHECK_EQ(markdownOf(t, EmptyFields()), "# Hi, \n\nYou have **** new .\n");
}

TEST_CASE("Markdown templates: a page with no fields is its source, exactly") {
    std::string source = "# Title\n\nSome *text*, a [link](other.md), `code {{x}}`, and \\{{ braces.\n\n<b>raw</b>\n";
    CompiledTemplate t = md(source, TemplateKind::Page);
    CHECK_FALSE(t.hasFields());
    CHECK_EQ(markdownOf(t, EmptyFields()), source);
    std::string html = htmlOf(t, EmptyFields());
    CHECK(contains(html, "<code>code {{x}}</code>"));
    CHECK(contains(html, "and {{ braces."));
    CHECK(contains(html, "&lt;b&gt;raw&lt;/b&gt;"));
    CHECK_EQ(pageTitle(t, EmptyFields()), "Title");
}

TEST_CASE("Markdown templates: fields are never recognized in code") {
    CompiledTemplate t = md("`{{a}}` and {{b}}\n\n```\n{{c}} {{ not a field }} [[x]]\n```\n\n    {{d}}\n");
    CHECK_EQ(t.fieldNames(), std::vector<std::string>{"b"});
    MapFields f = one("b", "B");
    CHECK_EQ(inner(htmlOf(t, f)), "<p><code>{{a}}</code> and B</p>\n<pre><code>{{c}} {{ not a field }} [[x]]\n"
                                  "</code></pre>\n<pre><code>{{d}}\n</code></pre>\n");
    CHECK_EQ(markdownOf(t, f), "`{{a}}` and B\n\n```\n{{c}} {{ not a field }} [[x]]\n```\n\n    {{d}}\n");
}

TEST_CASE("Markdown templates: the title") {
    CompiledTemplate t = md("Intro\n\n# Welcome, {{user.name}}!\n\n# Second\n", TemplateKind::Page);
    MapFields f;
    f.set("user", FieldValue::datum(Datum::map({{"name", Datum::string("<Bo>")}})));
    CHECK_EQ(pageTitle(t, f), "Welcome, <Bo>!");   // plain text; the layout escapes it
    CHECK_EQ(pageTitle(t, EmptyFields()), "Welcome, !");
}

TEST_CASE("Markdown templates: links and images") {
    CompiledTemplate t = md("[{{label}}]({{url}} \"{{tip}}\") [next](list.md?page={{n}}&x=1) "
                            "![{{alt}}](/img/{{file}})\n");
    MapFields f;
    f.set("label", FieldValue::string("a]b"));
    f.set("url", FieldValue::string("https://e.com/a b(c)"));
    f.set("tip", FieldValue::string("say \"hi\""));
    f.set("n", FieldValue::string("2&3 /"));
    f.set("alt", FieldValue::string("a \"cat\""));
    f.set("file", FieldValue::string("../x y.png"));
    CHECK_EQ(inner(htmlOf(t, f)),
             "<p><a href=\"https://e.com/a%20b(c)\" title=\"say &quot;hi&quot;\">a]b</a> "
             "<a href=\"list.md?page=2%263%20%2F&amp;x=1\">next</a> "
             "<img src=\"/img/..%2Fx%20y.png\" alt=\"a &quot;cat&quot;\"></p>\n");
    CHECK_EQ(markdownOf(t, f),
             "[a\\]b](https://e.com/a%20b%28c%29 \"say \\\"hi\\\"\") [next](list.md?page=2%263%20%2F&x=1) "
             "![a \\\"cat\\\"](/img/..%2Fx%20y.png)\n");

    // A URL that breaks the rule is replaced, in both forms.
    MapFields bad = one("url", "javascript:alert(1)");
    CHECK(contains(htmlOf(t, bad), "href=\"about:invalid#fourfold\""));
    CHECK(contains(markdownOf(t, bad), "](about:invalid#fourfold \""));
    RenderResult r = renderAs(t, bad, OutputFormat::Markdown);
    bool warned = false;
    for (const RenderWarning& w : r.warnings) warned = warned || contains(w.message, "isn't a safe URL");
    CHECK(warned);
}

TEST_CASE("Markdown templates: a field alone on a line is a block") {
    CompiledTemplate t = md("# Entries\n\n{{entries}}\n\nThe end.\n");
    // A string is a paragraph.
    MapFields s = one("entries", "*none*");
    CHECK_EQ(inner(htmlOf(t, s)), "<h1 id=\"entries\">Entries</h1>\n<p>*none*</p>\n<p>The end.</p>\n");
    CHECK_EQ(markdownOf(t, s), "# Entries\n\n\\*none\\*\n\nThe end.\n");
    // Rendered Markdown is its own blocks, in either form.
    MarkdownResult value = markdownValue("- one\n- **two**\n");
    REQUIRE(value.ok());
    MapFields m;
    m.set("entries", FieldValue::markup(value.html));
    CHECK_EQ(inner(htmlOf(t, m)), "<h1 id=\"entries\">Entries</h1>\n<div class=\"markdown limited\">\n<ul>\n"
                                  "<li>one</li>\n<li><strong>two</strong></li>\n</ul>\n</div>\n<p>The end.</p>\n");
    CHECK_EQ(markdownOf(t, m), "# Entries\n\n- one\n- **two**\n\nThe end.\n");
    // Null is nothing.
    MapFields n;
    n.set("entries", FieldValue::null());
    CHECK_EQ(markdownOf(t, n), "# Entries\n\n\n\nThe end.\n");
    // A block value right after a heading gets its blank line.
    CompiledTemplate tight = md("# Entries\n{{entries}}\n");
    CHECK_EQ(markdownOf(tight, m), "# Entries\n\n- one\n- **two**\n");
}

TEST_CASE("Markdown templates: HTML, and rendered values in text, are errors in both forms") {
    CompiledTemplate block = md("{{x}}\n");
    CompiledTemplate text = md("Hello {{x}}\n");
    MapFields html;
    html.set("x", FieldValue::markup(unsafeHtml("<b>hi</b>")));
    MarkdownResult value = markdownValue("hi");
    MapFields mdValue;
    mdValue.set("x", FieldValue::markup(value.html));
    for (OutputFormat format : {OutputFormat::Html, OutputFormat::Markdown}) {
        RenderResult r = renderAs(block, html, format);
        REQUIRE_FALSE(r.ok());
        CHECK(contains(r.error->message, "is HTML (from an HTML fragment or unsafeHTML)"));
        r = renderAs(text, mdValue, format);
        REQUIRE_FALSE(r.ok());
        CHECK(contains(r.error->message, "must be a paragraph of its own"));
        CHECK(renderAs(block, mdValue, format).ok());
    }
    // An HTML template has no Markdown form.
    ParsedTemplate h = parseFragment("<p>{{x}}</p>");
    REQUIRE(h.ok());
    CHECK_FALSE(renderAs(h.compiled, EmptyFields(), OutputFormat::Markdown).ok());
}

TEST_CASE("Markdown templates: includes") {
    FragmentMap frags;
    frags["note"] = md("> A note for *{{who}}*.");
    ParsedTemplate h = parseFragment("<b>html</b>");
    REQUIRE(h.ok());
    frags["html"] = std::move(h.compiled);
    REQUIRE(resolveIncludes(frags).empty());

    CompiledTemplate page = md("# Page\n{{> note}}\n\nAfter.\n", TemplateKind::Page);
    CHECK(resolveIncludes(page, frags).empty());
    CHECK_EQ(page.fieldNames(), std::vector<std::string>{"who"});
    MapFields f = one("who", "you");
    CHECK_EQ(inner(htmlOf(page, f)), "<h1 id=\"page\">Page</h1>\n<div class=\"markdown\">\n<blockquote>\n"
                                     "<p>A note for <em>you</em>.</p>\n</blockquote>\n</div>\n<p>After.</p>\n");
    CHECK_EQ(markdownOf(page, f), "# Page\n\n> A note for *you*.\n\nAfter.\n");

    // A Markdown file includes only Markdown.
    CompiledTemplate bad = md("{{> html}}\n");
    std::vector<TemplateProblem> problems = resolveIncludes(bad, frags);
    REQUIRE_EQ(problems.size(), 1);
    CHECK(contains(problems[0].message, "is HTML (code/frags/t.html), but a Markdown file includes only Markdown"));

    // An HTML page includes a Markdown fragment's HTML form.
    ParsedTemplate html = parsePage("<!DOCTYPE html><html><head></head><body>{{> note}}</body></html>");
    REQUIRE(html.ok());
    CHECK(resolveIncludes(html.compiled, frags).empty());
    CHECK(html.compiled.hasMarkdown());
    RenderOptions o;
    o.markdownCss = "/__fourfold/markdown.css";
    CHECK_EQ(render(html.compiled, f, o).html.str(),
             "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"/__fourfold/markdown.css\">\n</head><body>"
             "<div class=\"markdown\">\n<blockquote>\n<p>A note for <em>you</em>.</p>\n</blockquote>\n</div>\n"
             "</body></html>");
}

TEST_CASE("Markdown templates: where fields can't go") {
    struct Case {
        const char* source;
        const char* error;
    };
    const Case cases[] = {
        {"Hi\n{{> note}}\n", "must be a paragraph of its own"},
        {"- {{> note}}\n", "must be a paragraph of its own"},
        {"> {{> note}}\n", "must be a paragraph of its own"},
        {"text {{> note}}\n", "must be a paragraph of its own"},
        {"[x]: {{url}}\n\n[a][x]\n", "link reference definition"},
        {"[x]: /a/{{url}}\n\n[a][x] [b][x]\n", "link reference definition"},
        {"[{{x}}]: /a\n", "link reference definition"},
        {"[a][{{ref}}]\n\n[b]: /x\n", "inside [brackets] that aren't a link"},
        {"See [{{name}}].\n", "inside [brackets] that aren't a link"},
        {"[a [{{x}}] b](/u)\n", "inside [brackets] that aren't a link"},
        {"<https://e.com/{{x}}>\n", "autolink"},
        {"```{{lang}}\nx\n```\n", "info string"},
        {"Go to https://e.com/{{path}} now\n", "bare web or email address"},
        {"www.{{host}}.com\n", "bare web or email address"},
        {"{{user}}@example.com\n", "bare web or email address"},
        {"<{{tag}}>\n", "can't follow '<'"},
        {"<span title=\"{{x}}\">a</span>\n", "inside raw HTML"},
        {"<div>\n{{x}}\n</div>\n", "inside raw HTML"},
        {"[a](javascript:{{x}})\n", "whose URL isn't allowed"},
        {"Hello {{ name\n", "malformed field"},
        {"{{>}}\n", "malformed include"},
        {"a [[b]] c\n", "[[ is reserved"},
        {"[[x]]: /u\n", "[[ is reserved"},
        {"x \xEF\xB7\x90 y\n", "noncharacter"},
    };
    for (const Case& c : cases) {
        INFO(c.source);
        std::string error = mdError(c.source);
        CHECK_MESSAGE(contains(error, c.error), error);
    }
    // These are fine.
    const char* fine[] = {
        "\\[[ is literal, `[[` too\n",
        "\\[{{x}}\\]\n",
        "[{{x}}](/u) and ![{{x}}](/i.png)\n",
        "[a]({{u}}) [b](</x y/{{p}}>)\n",
        "Email {{user}}\\@example.com\n",
        "\\<{{x}}\n",
        "```\n<{{x}}> [[ {{ bad }}\n```\n",
        "| a | b |\n|---|---|\n| {{x}} | {{y}} |\n",
        "{{x}}.\n",
    };
    for (const char* f : fine) {
        INFO(f);
        CHECK_EQ(mdError(f), "");
    }
}

TEST_CASE("Markdown templates: errors say where") {
    ParsedTemplate p = parseMd("line one\n\nline [[three]]\n");
    REQUIRE_FALSE(p.ok());
    CHECK_EQ(p.errors[0].file, "frags/t.md");
    CHECK_EQ(p.errors[0].line, 3);
}

TEST_CASE("escapeMarkdownText") {
    CHECK_EQ(escapeMarkdownText("plain words"), "plain words");
    CHECK_EQ(escapeMarkdownText("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"),
             "\\!\\\"\\#\\$\\%\\&\\'\\(\\)\\*\\+\\,\\-\\.\\/\\:\\;\\<\\=\\>\\?\\@\\[\\\\\\]\\^\\_\\`\\{\\|\\}\\~");
    CHECK_EQ(escapeMarkdownText("a\r\nb\nc\rd\te"), "a b c d e");
    CHECK_EQ(escapeMarkdownText("    code  "), "&#32;&#32;&#32;&#32;code&#32;&#32;");
    CHECK_EQ(escapeMarkdownText("\nx"), "&#32;x");
    CHECK_EQ(escapeMarkdownText("   "), "&#32;&#32;&#32;");
    CHECK_EQ(escapeMarkdownText("www", true), "ww&#119;");
    CHECK_EQ(escapeMarkdownText("WWW", true), "WW&#87;");
    CHECK_EQ(escapeMarkdownText("www", false), "www");
    CHECK_EQ(markdownDestination("/a b(c)<d>"), "/a%20b%28c%29%3Cd%3E");
}

// The exit criterion: hostile values in every kind of place a field can be.
// The Markdown output, parsed again, must have the template's structure,
// with each value only in text: the same HTML as the HTML output.
TEST_CASE("Markdown templates: hostile values keep the structure, in both outputs") {
    const char* templates[] = {
        "Hello {{v}}!\n",
        "**{{v}}** and *{{v}}* and ~~{{v}}~~\n",
        "Intro\n\n{{v}}\n\nOutro\n",
        "- item {{v}}\n- two\n",
        "1. {{v}}\n",
        "> {{v}}\n",
        "| a | b |\n|---|---|\n| {{v}} | x |\n",
        "[{{v}}](/x)\n",
        "[t](/x?q={{v}})\n",
        "![{{v}}](/i.png)\n",
        "[t](/x \"{{v}}\")\n",
        "Visit {{v}}.com\n",
        "a {{v}}\nb\n",
        "{{v}}\n===\n",
        "x\n{{v}}\n",
        "{{v}}",
    };
    const char* values[] = {
        "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
        "line one\nline two\r\nthree",
        "    indented code",
        "trailing  ",
        "# heading",
        "- list",
        "1. list",
        "1) list",
        "> quote",
        "---",
        "===",
        "***",
        "| a | b |",
        "|---|",
        "[x](javascript:alert(1))",
        "![i](https://evil.com/i.png)",
        "<script>alert(1)</script>",
        "<img src=x onerror=alert(1)>",
        "&amp; &#106; &copy;",
        "www.evil.com",
        "www",
        "http://evil.com/x",
        "https://evil.com",
        "mailto:a@b.com",
        "a@b.com",
        "*em* _u_ `c` ~~d~~",
        "\\",
        "x\\",
        "[x]: /evil",
        "[[x]]",
        "{{x}}",
        "```js",
        "~~~",
        "<!-- c -->",
        "  ",
        // (An empty value is left out: it can leave a template's own markers
        // unpaired, like "****" for **{{v}}**, which then show as text.)
        "caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x98\x80",
    };
    auto normalize = [](std::string s) {
        // Line breaks become spaces in Markdown text (and HTML collapses
        // them anyway); the heading's id is from the template, without the
        // value.
        s = std::regex_replace(s, std::regex("[ \t\r\n]+"), " ");
        s = std::regex_replace(s, std::regex(" ?(<[^>]*>) ?"), "$1");
        s = std::regex_replace(s, std::regex(" id=\"[^\"]*\""), "");
        return s;
    };
    for (const char* source : templates) {
        CompiledTemplate t = md(source);
        for (const char* value : values) {
            INFO("template: ", source);
            INFO("value: ", value);
            MapFields f = one("v", value);
            std::string html = htmlOf(t, f);
            std::string markdown = markdownOf(t, f);
            INFO("markdown: ", markdown);
            MarkdownOptions full;
            full.profile = MarkdownProfile::Full;
            MarkdownResult again = renderMarkdown(markdown, full);
            REQUIRE(again.ok());
            CHECK_EQ(normalize(again.html.str()), normalize(html));
        }
    }
}

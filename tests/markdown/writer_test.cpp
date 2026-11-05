// The Markdown writer: markdown() values written back out as Markdown, for
// a Markdown page's Markdown output (M6.1).

#include <regex>

#include "doctest.h"
#include "markdown/markdown.h"

using namespace fourfold;

namespace {

std::string html(const std::string& text, MarkdownProfile profile) {
    MarkdownOptions o;
    o.profile = profile;
    MarkdownResult r = renderMarkdown(text, o);
    REQUIRE(r.ok());
    return r.html.str();
}

std::string normalize(std::string s) {
    s = std::regex_replace(s, std::regex("[ \t\r\n]+"), " ");
    return std::regex_replace(s, std::regex(" ?(<[^>]*>) ?"), "$1");
}

// Written out and parsed again, the text renders as it did.
void roundTrip(const std::string& text) {
    for (MarkdownProfile profile : {MarkdownProfile::Limited, MarkdownProfile::Full}) {
        std::string written = writeMarkdown(text, profile);
        INFO("text: ", text);
        INFO("profile: ", profile == MarkdownProfile::Full ? "full" : "limited");
        INFO("written: ", written);
        CHECK_EQ(normalize(html(written, profile)), normalize(html(text, profile)));
        // Written again, it's the same.
        CHECK_EQ(writeMarkdown(written, profile), written);
    }
}

std::string limited(const std::string& text) {
    return writeMarkdown(text, MarkdownProfile::Limited);
}

std::string full(const std::string& text) {
    return writeMarkdown(text, MarkdownProfile::Full);
}

}  // namespace

TEST_CASE("writeMarkdown: ordinary Markdown comes out readable") {
    CHECK_EQ(full("Hello, *world*. It's **nice**, isn't it?"), "Hello, *world*. It's **nice**, isn't it?\n");
    CHECK_EQ(full("# Title\n\nText\nmore text"), "# Title\n\nText\nmore text\n");
    CHECK_EQ(full("- a\n- b\n  - c"), "- a\n- b\n  - c\n");
    CHECK_EQ(full("3. x\n4. y"), "3. x\n4. y\n");
    CHECK_EQ(full("> quoted\n> more"), "> quoted\n> more\n");
    CHECK_EQ(full("Some `code` here"), "Some `code` here\n");
    CHECK_EQ(full("    indented\n    code"), "```\nindented\ncode\n```\n");
    CHECK_EQ(full("```js\nlet a = `x`;\n```"), "```js\nlet a = `x`;\n```\n");
    CHECK_EQ(full("[a link](https://e.com \"T\") and ![an image](/i.png)"),
             "[a link](https://e.com \"T\") and ![an image](/i.png)\n");
    CHECK_EQ(full("| a | b |\n|:--|--:|\n| 1 | 2 |"), "| a | b |\n| :-- | --: |\n| 1 | 2 |\n");
    CHECK_EQ(full("- [x] done\n- [ ] not"), "- [x] done\n- [ ] not\n");
    CHECK_EQ(full("a\n\n***\n\nb"), "a\n\n***\n\nb\n");
    CHECK_EQ(full("line  \nbreak"), "line\\\nbreak\n");
}

TEST_CASE("writeMarkdown: reference links become inline links") {
    CHECK_EQ(full("[a][x] and [x]\n\n[x]: https://e.com \"T\""),
             "[a](https://e.com \"T\") and [x](https://e.com \"T\")\n");
}

TEST_CASE("writeMarkdown: Limited Markdown's rules") {
    CHECK_EQ(limited("# Big\n\ntext"), "Big\n\ntext\n");
    CHECK_EQ(limited("![cat](https://e.com/c.png)"), "[cat](https://e.com/c.png)\n");
    CHECK_EQ(limited("![](https://e.com/c.png)"), "[https:\\//e.com/c.png](https://e.com/c.png)\n");
    CHECK_EQ(limited("[![cat](/c.png)](/x)"), "[cat](/x)\n");   // not a link in a link
    CHECK_EQ(limited("# "), "");   // an empty heading: nothing
    CHECK_EQ(limited("[x](javascript:alert(1)) and ![y](data:x)"), "x and y\n");
    CHECK_EQ(limited("<script>alert(1)</script>"), "\\<script\\>alert(1)\\</script\\>\n");
    CHECK_EQ(limited("<b>x</b> &amp; &#106;"), "\\<b\\>x\\</b\\> &amp; &#106;\n");
    CHECK_EQ(limited("see www.e.com and a@b.com"),
             "see [www\\.e.com](http://www.e.com) and [a\\@b.com](mailto:a@b.com)\n");
}

TEST_CASE("writeMarkdown: text that would mean something is escaped") {
    CHECK_EQ(full("\\# not a heading"), "\\# not a heading\n");
    CHECK_EQ(full("1\\. not a list"), "1\\. not a list\n");
    CHECK_EQ(full("\\- not a list"), "\\- not a list\n");
    CHECK_EQ(full("a\n\\=\\=\\="), "a\n\\===\n");
    CHECK_EQ(full("\\*a\\* \\_b\\_ \\`c\\` \\[d\\](e) \\<f\\> \\|"), "\\*a\\* \\_b\\_ \\`c\\` \\[d\\](e) \\<f\\> \\|\n");
    CHECK_EQ(full("www\\.e.com, http:\\//e.com, a\\@b.com"), "www\\.e.com, http:\\//e.com, a\\@b.com\n");
    CHECK_EQ(full("!\\[x](y)"), "!\\[x\\](y)\n");
    CHECK_EQ(full("Hi\\![x](https://e.com)"), "Hi\\![x](https://e.com)\n");   // not an image
    CHECK_EQ(full("[a](/x_(y))"), "[a](</x_(y)>)\n");
}

TEST_CASE("writeMarkdown: round trips") {
    const char* corpus[] = {
        "Hello, *world*.",
        "# H1\n## H2 with `code`\n### H3 #",
        "Setext\n===\n\nTwo\n---",
        "- a\n- b\n\n  para in b\n- c",
        "1. one\n2. two\n   - nested\n   - list\n3. three",
        "1) paren\n2) list",
        "* star\n* list\n\n+ plus\n+ list",
        "> quote\n> > nested\n> back",
        "> - list in quote\n> - two",
        "- > quote in list\n- two",
        "```\ncode with ``` inside? no, ~~~\n```",
        "````\n```\n````",
        "~~~py\nx = 1\n~~~",
        "`` a`b ``, ` `` `, `  `",
        "**strong *em* strong** and ***both***",
        "*em **strong** em*",
        "~~del~~ and ~~*em del*~~",
        "a_b_c and a*b*c and _a_",
        "[link *em*](https://e.com/(x)) and <https://e.com/a b>",
        "[x](</a b> \"t \\\" q\")",
        "![img *alt*](/i.png \"t\") and [![img](/i.png)](/link)",
        "| h1 | h2 |\n|---|:-:|\n| `a|b` | **c** |\n| d |",
        "- [x] task\n- [ ] open\n\n  more",
        "line\\\nbreak and line  \nbreak",
        "&copy; &#169; &amp;amp; &lt;b&gt;",
        "<div>html</div>\n\n<!-- c --> <b>x</b>",
        "trailing \\\\",
        "a\n\n\n\nb",
        "- \n- x",
        ">",
        "[a]: /x\n\n[a] [b][a] [a][]",
        "www.e.com/x?y=1, https://e.com., x@e.com",
        "javascript:alert(1) [x](javascript:alert(1)) [y](JaVaScRiPt:x)",
        "1. a\n\n   ```\n   code\n   ```\n2. b",
        "Text\n\n    indented\n\nmore",
        "* * *\n- - -\n___",
        "a  \n  b",
        "\\\\\\*",
        "10. ten\n11. eleven",
        "- a\n\n- b",
        "text www.e.com text",
        "caf\xC3\xA9 \xF0\x9F\x98\x80 \xE2\x80\x94",
    };
    for (const char* text : corpus) roundTrip(text);
}

TEST_CASE("markdownValue: its Markdown form is written when asked for") {
    MarkdownResult r = markdownValue("# Hi *there*");
    REQUIRE(r.ok());
    CHECK(r.html.isMarkdown());
    CHECK_EQ(r.html.str(), "<div class=\"markdown limited\">\n<p>Hi <em>there</em></p>\n</div>\n");
    REQUIRE(r.html.markdown());
    CHECK_EQ(*r.html.markdown(), "Hi *there*\n");
    MarkdownOptions full;
    full.profile = MarkdownProfile::Full;
    CHECK_EQ(*markdownValue("# Hi", full).html.markdown(), "# Hi\n");
    // HTML isn't Markdown.
    CHECK_FALSE(unsafeHtml("<b>x</b>").isMarkdown());
    Html joined = r.html;
    joined += markdownValue("two").html;
    CHECK_EQ(*joined.markdown(), "Hi *there*\n\ntwo\n");
    joined += unsafeHtml("<i>x</i>");
    CHECK_FALSE(joined.isMarkdown());
}

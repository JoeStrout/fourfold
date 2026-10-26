#include "markdown/markdown.h"

#include "doctest.h"

using namespace fourfold;

namespace {

std::string full(const std::string& text) {
    MarkdownOptions o;
    o.profile = MarkdownProfile::Full;
    MarkdownResult r = renderMarkdown(text, o);
    REQUIRE(r.ok());
    return r.html.str();
}

std::string comment(const std::string& text) {
    MarkdownResult r = renderMarkdown(text);
    REQUIRE(r.ok());
    return r.html.str();
}

// The output without its wrapper.
std::string inner(const std::string& html) {
    size_t start = html.find('\n') + 1;
    return html.substr(start, html.size() - start - std::string("</div>\n").size());
}

std::string fullInner(const std::string& text) {
    return inner(full(text));
}

std::string commentInner(const std::string& text) {
    return inner(comment(text));
}

}  // namespace

TEST_CASE("markdown: the wrapper, and the profiles' names") {
    CHECK_EQ(full("hi"), "<div class=\"markdown\">\n<p>hi</p>\n</div>\n");
    CHECK_EQ(comment("hi"), "<div class=\"markdown comment\">\n<p>hi</p>\n</div>\n");
    CHECK_EQ(markdownProfileNamed("comment"), MarkdownProfile::Comment);
    CHECK_EQ(markdownProfileNamed("full"), MarkdownProfile::Full);
    CHECK_FALSE(markdownProfileNamed("raw"));
    CHECK(renderMarkdown("x").html.hasMarkdown());
    CHECK_FALSE(renderMarkdown("x").html.hasScript());
}

TEST_CASE("markdown: both profiles share the basics") {
    for (bool f : {true, false}) {
        auto r = [&](const std::string& t) { return f ? fullInner(t) : commentInner(t); };
        CHECK_EQ(r("a\nb"), "<p>a\nb</p>\n");
        CHECK_EQ(r("a  \nb"), "<p>a<br>\nb</p>\n");
        CHECK_EQ(r("**b** *i* ~~s~~ `c<d>`"), "<p><strong>b</strong> <em>i</em> <del>s</del> <code>c&lt;d&gt;</code></p>\n");
        CHECK_EQ(r("```js\nif (a < b) {}\n```"), "<pre><code class=\"language-js\">if (a &lt; b) {}\n</code></pre>\n");
        CHECK_EQ(r("```\"><script>\nx\n```"), "<pre><code>x\n</code></pre>\n");
        CHECK_EQ(r("    indented <code>"), "<pre><code>indented &lt;code&gt;\n</code></pre>\n");   // found by fuzzing
        CHECK_EQ(r("- a\n- b"), "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
        CHECK_EQ(r("3. a"), "<ol start=\"3\">\n<li>a</li>\n</ol>\n");
        CHECK_EQ(r("> q"), "<blockquote>\n<p>q</p>\n</blockquote>\n");
        CHECK_EQ(r("| a | b |\n|:--|--:|\n| 1 | 2 |"),
                 "<table>\n<thead>\n<tr><th align=\"left\">a</th><th align=\"right\">b</th></tr>\n</thead>\n"
                 "<tbody>\n<tr><td align=\"left\">1</td><td align=\"right\">2</td></tr>\n</tbody>\n</table>\n");
        CHECK_EQ(r("- [x] done\n- [ ] not"),
                 "<ul>\n<li class=\"task-list-item\"><input type=\"checkbox\" disabled checked> done</li>\n"
                 "<li class=\"task-list-item\"><input type=\"checkbox\" disabled> not</li>\n</ul>\n");
        CHECK_EQ(r("***"), "<hr>\n");
        CHECK_EQ(r("a &amp; &copy; &#106;"), "<p>a &amp; \xC2\xA9 j</p>\n");
    }
}

TEST_CASE("markdown: raw HTML is shown as text") {
    CHECK_EQ(commentInner("<script>alert(1)</script>"), "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n");
    CHECK_EQ(fullInner("<div onclick=\"x\">hi</div>"), "<p>&lt;div onclick=&quot;x&quot;&gt;hi&lt;/div&gt;</p>\n");
    CHECK_EQ(commentInner("a <img src=x onerror=alert(1)> b"),
             "<p>a &lt;img src=x onerror=alert(1)&gt; b</p>\n");
    CHECK_EQ(commentInner("<!-- comment -->"), "<p>&lt;!-- comment --&gt;</p>\n");
}

TEST_CASE("markdown: links follow the URL rule") {
    CHECK_EQ(fullInner("[x](https://example.com/a?b=1)"), "<p><a href=\"https://example.com/a?b=1\">x</a></p>\n");
    CHECK_EQ(fullInner("[x](/a \"T<\")"), "<p><a href=\"/a\" title=\"T&lt;\">x</a></p>\n");
    CHECK_EQ(commentInner("[x](https://e.com)"), "<p><a href=\"https://e.com\" rel=\"nofollow ugc\">x</a></p>\n");
    const char* bad[] = {
        "[click](javascript:alert(1))",
        "[click](JAVASCRIPT:alert(1))",
        "[click](&#106;avascript:alert(1))",
        "[click](&#x6A;avascript:alert(1))",
        "[click](java&#09;script:alert(1))",
        "[click](data:text/html,x)",
        "[click](vbscript:x)",
        "<javascript:alert(1)>",
        "[click][r]\n\n[r]: javascript:alert(1)",
    };
    for (const char* b : bad) {
        INFO(b);
        std::string out = fullInner(b);
        CHECK(out.find("<a") == std::string::npos);
        CHECK(out.find("href") == std::string::npos);
    }
    CHECK_EQ(fullInner("[click](javascript:alert(1))"), "<p>click</p>\n");
    // Autolinks.
    CHECK_EQ(commentInner("see https://e.com/x"), "<p>see <a href=\"https://e.com/x\" rel=\"nofollow ugc\">https://e.com/x</a></p>\n");
    CHECK_EQ(fullInner("www.e.com"), "<p><a href=\"http://www.e.com\">www.e.com</a></p>\n");
    CHECK_EQ(fullInner("me@e.com"), "<p><a href=\"mailto:me@e.com\">me@e.com</a></p>\n");
    // Quotes and spaces in a destination are encoded.
    CHECK_EQ(fullInner("[x](</a b\"c>)"), "<p><a href=\"/a%20b%22c\">x</a></p>\n");
}

TEST_CASE("markdown: images") {
    CHECK_EQ(fullInner("![a *b*](/i.png \"t\")"), "<p><img src=\"/i.png\" alt=\"a b\" title=\"t\"></p>\n");
    CHECK_EQ(fullInner("![x\"y](javascript:x)"), "<p>x&quot;y</p>\n");
    // In comments, a link to the image instead.
    CHECK_EQ(commentInner("![cat](https://e.com/c.png)"),
             "<p><a href=\"https://e.com/c.png\" rel=\"nofollow ugc\">cat</a></p>\n");
    CHECK_EQ(commentInner("![](https://e.com/c.png)"),
             "<p><a href=\"https://e.com/c.png\" rel=\"nofollow ugc\">https://e.com/c.png</a></p>\n");
}

TEST_CASE("markdown: headings") {
    CHECK_EQ(fullInner("# Hello, World!\n## Hello, World!\n### Café & `code`"),
             "<h1 id=\"hello-world\">Hello, World!</h1>\n<h2 id=\"hello-world-1\">Hello, World!</h2>\n"
             "<h3 id=\"café--code\">Café &amp; <code>code</code></h3>\n");
    // Comments get no headings and no ids, so they can't clobber the page's.
    CHECK_EQ(commentInner("# entries\n## two"), "<p>entries</p>\n<p>two</p>\n");
    MarkdownOptions o;
    o.profile = MarkdownProfile::Full;
    CHECK_EQ(renderMarkdown("intro\n\n# The *Title*\n# Second", o).title, "The Title");
    CHECK_EQ(headingSlug("A b_c-D?"), "a-b_c-d");
}

TEST_CASE("markdown: rewriting URLs (full profile)") {
    MarkdownOptions o;
    o.profile = MarkdownProfile::Full;
    std::vector<std::string> seen;
    o.rewriteUrl = [&](const std::string& url, bool isImage, std::string& warning) -> std::optional<std::string> {
        seen.push_back(url + (isImage ? " (image)" : ""));
        if (url == "setup.md#x") return std::string("/setup#x");
        if (url == "gone.md") warning = "links to gone.md, which doesn't exist";
        return std::nullopt;
    };
    MarkdownResult r = renderMarkdown("[a](setup.md#x) [b](gone.md) ![c](i.png) [d](javascript:x)", o);
    REQUIRE(r.ok());
    CHECK_EQ(inner(r.html.str()), "<p><a href=\"/setup#x\">a</a> <a href=\"gone.md\">b</a> <img src=\"i.png\" alt=\"c\"> d</p>\n");
    CHECK_EQ(seen, std::vector<std::string>{"setup.md#x", "gone.md", "i.png (image)"});
    CHECK_EQ(r.warnings, std::vector<std::string>{"links to gone.md, which doesn't exist"});
    // Comments aren't rewritten.
    o.profile = MarkdownProfile::Comment;
    seen.clear();
    renderMarkdown("[a](setup.md)", o);
    CHECK(seen.empty());
}

TEST_CASE("markdown: the size cap") {
    MarkdownOptions o;
    o.maxBytes = 10;
    CHECK(renderMarkdown("0123456789", o).ok());
    MarkdownResult r = renderMarkdown("0123456789x", o);
    CHECK_FALSE(r.ok());
    CHECK(r.html.empty());
    CHECK(r.error->find("over the limit of 10") != std::string::npos);
}

TEST_CASE("markdown: NUL and invalid input don't break it") {
    CHECK_EQ(commentInner(std::string("a\0b", 3)), "<p>a\xEF\xBF\xBD" "b</p>\n");
    CHECK(renderMarkdown("\xFF\xFE [x](\xC0)").ok());
    CHECK(renderMarkdown("").ok());
}

// Fuzzing the template tokenizer and the Markdown renderer, deterministically
// (a fixed seed), so it can run with every test run. See CMakeLists.txt.
//
// - Mutated templates must parse or be refused without crashing, and when
//   they render, a hostile value must never come out unescaped.
// - Mutated Markdown must only ever produce the tags and attributes the
//   renderer writes, with safe URLs.
// - Adversarial inputs must take time linear in their size.
// - The Markdown writer (M6.1) must write nothing that a GFM client, which
//   renders raw HTML, would read as HTML, and in Limited Markdown no
//   headings or images, with safe URLs only.
// - Mutated Markdown templates must parse or be refused without crashing,
//   and when filled with hostile values, their Markdown output must keep the
//   values out of HTML and URLs.
// - Mutated form bodies (M7), url-encoded and multipart, must be read or
//   refused without crashing, within the limits, in linear time, and checked
//   against a declaration without crashing.

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../template/helpers.h"
#include "base/utf8.h"
#include "doctest.h"
#include "markdown/markdown.h"
#include "pipeline/forms.h"
#include "md4c.h"

using namespace fourfold;
using namespace fourfold::test;

namespace {

// SplitMix64: small, fast, and the same everywhere.
struct Rng {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

std::string mutate(std::string s, Rng& rng, const std::vector<std::string>& tokens) {
    int ops = 1 + static_cast<int>(rng.below(4));
    for (int k = 0; k < ops; k++) {
        size_t at = rng.below(s.size() + 1);
        switch (rng.below(5)) {
            case 0:
            case 1: s.insert(at, tokens[rng.below(tokens.size())]); break;
            case 2: s.erase(at, rng.below(8)); break;
            case 3: {   // repeat a piece
                size_t len = std::min(rng.below(12), s.size() - at);
                s.insert(at, s.substr(at, len));
                break;
            }
            default:
                if (!s.empty()) s[rng.below(s.size())] = static_cast<char>(rng.below(256));
        }
    }
    if (s.size() > 4000) s.resize(4000);
    return s;
}

// The minimum, over a few runs, of how long `f` takes, in milliseconds.
double timeOf(const std::function<void()>& f) {
    double best = 1e9;
    for (int i = 0; i < 3; i++) {
        auto start = std::chrono::steady_clock::now();
        f();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        best = std::min(best, ms);
    }
    return best;
}

// Requires that 4× the input takes well under 16× the time (quadratic), with
// a floor for timer noise. Linear growth is 4×; this allows for caches and
// allocation.
void checkLinear(const std::string& what, const std::function<std::string(size_t)>& input,
                 const std::function<void(const std::string&)>& run, size_t n) {
    std::string small = input(n), large = input(4 * n);
    double t1 = timeOf([&] { run(small); });
    double t4 = timeOf([&] { run(large); });
    INFO(what, ": ", small.size(), " bytes in ", t1, " ms; ", large.size(), " bytes in ", t4, " ms");
    CHECK(t4 < 8 * t1 + 5);
    if (std::getenv("FOURFOLD_FUZZ_TIMES")) {
        MESSAGE(what << ": " << small.size() << " bytes " << t1 << " ms, " << large.size() << " bytes " << t4
                     << " ms (x" << (t1 > 0 ? t4 / t1 : 0) << ")");
    }
}

std::string repeat(const std::string& s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; i++) out += s;
    return out;
}

// --- Templates ---

const char* const kHostile = "\"'<>&`/ javascript:x </script> {{y}} \xE2\x80\xA8";
const char* const kHostileCore = "\"'<>";   // must never appear as is in output

const std::vector<std::string> kTemplateSeeds = {
    "<p>{{a}}</p>",
    "<a href=\"{{u}}\" title='{{t}}' class=\"x {{c}}\">{{b}}</a>",
    "<input checked=\"{{on}}\" value=\"{{v}}\">",
    "<script>var a = {{a}}; if (x) /re/.test(y); s = \"str\"; t = `t${1}`;</script>{{b}}",
    "<title>{{t}}</title><textarea>{{x}}</textarea>",
    "<!-- c --><svg><path d=\"M0\"/></svg><p>{{a}}</p>",
    "<a href=\"/x?q={{q}}#{{h}}\">\\{{lit}}</a>",
    "<div hx-get=\"{{u}}\" hx-target=\"#t\">{{> inc}}</div>",
    "<!DOCTYPE html><html><head></head><body>{{a}}</body></html>",
    "<script type=\"application/json\">{\"a\": {{a}}}</script>",
};

const std::vector<std::string> kTemplateTokens = {
    "{{a}}", "{{u}}", "{{b.c}}", "\\{{", "{{> inc}}", "{{", "}}", "<", ">", "</", "/>", "\"", "'", "=", " ",
    "<!--", "-->", "<script>", "</script>", "<style>", "</style>", "<svg>", "</svg>", "<title>", "</title>",
    "<textarea>", "<a href=\"", "<a title=", "<b ", "onclick=", "style=", "`", "/", "//", "/*", "*/", "${", "}",
    "(", ")", "\n", "<!DOCTYPE html>", "<![CDATA[", "]]>", "&", "&amp;", std::string(1, '\0'), "\xE2\x80\xA8",
};

void checkTemplate(const std::string& text) {
    for (bool page : {false, true}) {
        ParsedTemplate p = page ? parsePage(text) : parseFragment(text);
        if (!p.ok()) continue;
        MapFields fields;
        for (const std::string& name : p.compiled.fieldNames()) fields.set(name, FieldValue::string(kHostile));
        RenderOptions options;
        options.nonce = "n";
        RenderResult r = render(p.compiled, fields, options);
        if (!r.ok()) continue;
        if (text.find(kHostileCore) != std::string::npos) continue;
        INFO("template: ", text);
        INFO("output: ", r.html.str());
        CHECK(r.html.str().find(kHostileCore) == std::string::npos);
        // A URL field never starts with the hostile value's scheme.
        CHECK(r.html.str().find("=\"javascript:") == std::string::npos);
    }
}

// --- Markdown ---

const std::vector<std::string> kMarkdownSeeds = {
    "# Title\n\nSome *em* and **strong** and `code` and ~~del~~.",
    "[link](https://e.com \"t\") ![img](/i.png) <https://auto.link> www.e.com me@e.com",
    "- a\n- [x] b\n  1. c\n\n> quote\n> > nested",
    "| a | b |\n|:-|-:|\n| 1 | 2 |",
    "```js\ncode < >\n```\n\n    indented",
    "<script>alert(1)</script> <b onclick=x>b</b> <!-- c -->",
    "[a][r]\n\n[r]: javascript:alert(1)",
    "&#106;avascript: &amp; &copy; &#0; &#x110000;",
    "***\n\nSetext\n===\n\nLine  \nbreak",
};

const std::vector<std::string> kMarkdownTokens = {
    "[", "]", "(", ")", "![", "<", ">", "*", "**", "_", "`", "```", "~~", "#", "\n", "\n\n", "- ", "1. ", "> ",
    "|", "|-|", "&", "&#106;", "javascript:", "JaVaScRiPt:", "data:", "http://", "\\", "\"", "'", " ", "    ",
    "<script>", "</script>", "<img src=x onerror=y>", "[x]: ", std::string(1, '\0'), "www.", "@", "mailto:",
};

const std::set<std::string> kAllowedTags = {"div", "p",  "h1",    "h2",    "h3",    "h4",    "h5", "h6",
                                            "ul",  "ol", "li",    "blockquote", "pre", "code", "em", "strong",
                                            "del", "a",  "img",   "br",    "hr",    "table", "thead", "tbody",
                                            "tr",  "th", "td",    "input"};
const std::set<std::string> kAllowedAttributes = {"class", "id",    "href",  "title",    "rel",    "src",
                                                  "alt",   "start", "align", "type",     "disabled", "checked"};

// The URL rule's replacement, perhaps with the rest of a template's URL
// after it: inert.
bool startsInvalid(const std::string& url) {
    return url.compare(0, std::char_traits<char>::length(kInvalidUrl), kInvalidUrl) == 0;
}

// Every tag in the renderer's output is one it writes, with only its
// attributes, and URLs that pass the rule.
void checkMarkdownOutput(const std::string& html, const std::string& source) {
    INFO("markdown: ", source);
    INFO("output: ", html);
    for (size_t i = html.find('<'); i != std::string::npos; i = html.find('<', i + 1)) {
        size_t j = i + 1;
        if (j < html.size() && html[j] == '/') j++;
        size_t nameStart = j;
        while (j < html.size() && std::isalnum(static_cast<unsigned char>(html[j]))) j++;
        std::string name = html.substr(nameStart, j - nameStart);
        REQUIRE_MESSAGE(kAllowedTags.count(name), "tag <" << name << ">");
        size_t end = html.find('>', j);
        REQUIRE(end != std::string::npos);
        // Attributes: name="value" pairs (values are escaped, so no '>' or '"').
        std::string attrs = html.substr(j, end - j);
        size_t k = 0;
        while (k < attrs.size()) {
            while (k < attrs.size() && attrs[k] == ' ') k++;
            if (k >= attrs.size()) break;
            size_t eq = k;
            while (eq < attrs.size() && attrs[eq] != '=' && attrs[eq] != ' ') eq++;
            std::string attr = attrs.substr(k, eq - k);
            CHECK_MESSAGE(kAllowedAttributes.count(attr), "attribute " << attr);
            if (eq < attrs.size() && attrs[eq] == '=') {
                REQUIRE(attrs[eq + 1] == '"');
                size_t close = attrs.find('"', eq + 2);
                REQUIRE(close != std::string::npos);
                std::string value = attrs.substr(eq + 2, close - eq - 2);
                if ((attr == "href" || attr == "src") && !startsInvalid(value)) {
                    std::string scheme = urlScheme(value);
                    CHECK_MESSAGE((scheme.empty() || scheme == "http" || scheme == "https" || scheme == "mailto"),
                                  "URL " << value);
                }
                k = close + 1;
            } else {
                k = eq;
            }
        }
        i = end;
    }
}

void checkMarkdown(const std::string& text) {
    for (auto profile : {MarkdownProfile::Limited, MarkdownProfile::Full}) {
        MarkdownOptions options;
        options.profile = profile;
        MarkdownResult r = renderMarkdown(text, options);
        REQUIRE(r.ok());
        checkMarkdownOutput(r.html.str(), text);
        if (profile == MarkdownProfile::Limited) {
            CHECK(r.html.str().find("<img") == std::string::npos);
            CHECK(r.html.str().find(" id=") == std::string::npos);
            for (const char* h : {"<h1", "<h2", "<h3", "<h4", "<h5", "<h6"}) {
                CHECK(r.html.str().find(h) == std::string::npos);
            }
        }
    }
}

// --- Markdown output, as a GFM client reads it ---

// What a generic GFM parser, with raw HTML on, finds in a Markdown text.
struct GfmScan {
    bool html = false;           // raw HTML, inline or a block
    bool heading = false;
    bool image = false;
    std::vector<std::string> urls;
    std::string text;            // all of its text

    static std::string attribute(const MD_ATTRIBUTE& a) { return std::string(a.text, a.size); }

    explicit GfmScan(const std::string& markdown) {
        MD_PARSER p{};
        p.flags = MD_DIALECT_GITHUB;
        p.enter_block = [](MD_BLOCKTYPE type, void*, void* user) {
            auto* s = static_cast<GfmScan*>(user);
            if (type == MD_BLOCK_HTML) s->html = true;
            if (type == MD_BLOCK_H) s->heading = true;
            return 0;
        };
        p.leave_block = [](MD_BLOCKTYPE, void*, void*) { return 0; };
        p.enter_span = [](MD_SPANTYPE type, void* detail, void* user) {
            auto* s = static_cast<GfmScan*>(user);
            if (type == MD_SPAN_A) s->urls.push_back(attribute(static_cast<MD_SPAN_A_DETAIL*>(detail)->href));
            if (type == MD_SPAN_IMG) {
                s->image = true;
                s->urls.push_back(attribute(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src));
            }
            return 0;
        };
        p.leave_span = [](MD_SPANTYPE, void*, void*) { return 0; };
        p.text = [](MD_TEXTTYPE type, const MD_CHAR* chars, MD_SIZE size, void* user) {
            auto* s = static_cast<GfmScan*>(user);
            if (type == MD_TEXT_HTML) s->html = true;
            s->text.append(chars, size);
            return 0;
        };
        md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &p, this);
    }
};

bool safeUrl(const std::string& url) {
    std::string scheme = urlScheme(url);
    return scheme.empty() || scheme == "http" || scheme == "https" || scheme == "mailto" ||
           // GFM's own autolinks: "www.x" becomes http://www.x.
           url.compare(0, 7, "http://") == 0;
}

void checkWriter(const std::string& text) {
    for (auto profile : {MarkdownProfile::Limited, MarkdownProfile::Full}) {
        std::string written = writeMarkdown(text, profile);
        INFO("markdown: ", text);
        INFO("written: ", written);
        GfmScan scan(written);
        CHECK_FALSE(scan.html);
        for (const std::string& url : scan.urls) CHECK_MESSAGE(safeUrl(url), "URL " << url);
        if (profile == MarkdownProfile::Limited) {
            CHECK_FALSE(scan.heading);
            CHECK_FALSE(scan.image);
        }
        MarkdownOptions options;
        options.profile = profile;
        MarkdownResult r = renderMarkdown(written, options);
        REQUIRE(r.ok());
        checkMarkdownOutput(r.html.str(), written);
    }
}

// --- Markdown templates ---

const char* const kMarkdownHostile = "<img src=x onerror=alert(1)> [a](javascript:alert(1)) ![i](http://evil/i) "
                                     "</p>\n# h\n    code\n- li\n| a |";

const std::vector<std::string> kMarkdownTemplateSeeds = {
    "# Hi, {{name}}\n\nYou have **{{count}}** notes.",
    "[{{label}}]({{url}} \"{{tip}}\") and ![{{alt}}](/i/{{file}})",
    "- {{a}}\n- *{{b}}*\n\n> {{c}}",
    "| {{a}} | b |\n|---|---|\n| c | {{d}} |",
    "Intro\n\n{{entries}}\n\nOutro",
    "{{> note}}\n\n`{{code}}` and \\{{literal}}",
    "```\n{{in code}}\n```\n\n{{x}}",
};

const std::vector<std::string> kMarkdownTemplateTokens = {
    "{{a}}", "{{u}}", "{{b.c}}", "\\{{", "{{> note}}", "{{", "}}", "[[", "\\[[", "[", "]", "(", ")", "![",
    "<", ">", "*", "_", "`", "```", "#", "\n", "\n\n", "- ", "1. ", "> ", "|", "|-|", "&", "\\", "\"", " ",
    "    ", "<b>", "[x]: ", "www.", "@", "://", "javascript:", std::string(1, '\0'), "\xEF\xB7\x90",
};

void checkMarkdownTemplate(const std::string& text) {
    ParsedTemplate p = parseMarkdownTemplate(codeFile("frags/m.md", text), TemplateKind::Fragment);
    if (!p.ok()) return;
    // (An include stays unresolved here, and renders as nothing.)
    MapFields fields;
    for (const std::string& name : p.compiled.fieldNames()) fields.set(name, FieldValue::string(kMarkdownHostile));
    RenderOptions html, md;
    md.format = OutputFormat::Markdown;
    RenderResult h = render(p.compiled, fields, html);
    RenderResult m = render(p.compiled, fields, md);
    CHECK_EQ(h.ok(), m.ok());
    if (!h.ok() || !m.ok()) return;   // e.g. {{b.c}}, with b a string
    INFO("template: ", text);
    INFO("markdown: ", m.markdown);
    checkMarkdownOutput(h.html.str(), text);
    // The value's tag and link never come out as HTML or a link in either.
    CHECK(h.html.str().find("<img src=x") == std::string::npos);
    GfmScan scan(m.markdown);
    GfmScan source(text);
    if (!source.html) CHECK_FALSE(scan.html);   // (raw HTML of the template's own is sent as written)
    // A URL is the template's, or the URL rule's replacement, with any value
    // in it percent-encoded.
    for (const std::string& url : scan.urls) {
        CHECK_MESSAGE((safeUrl(url) || startsInvalid(url)), "URL " << url);
        CHECK_MESSAGE(url.find("evil/") == std::string::npos, "URL " << url);
    }
}

}  // namespace

TEST_CASE("fuzz: mutated templates") {
    Rng rng{0xF0F0F0F0u};
    int parsed = 0, total = 0;
    for (const std::string& seed : kTemplateSeeds) checkTemplate(seed);
    for (int i = 0; i < 3000; i++) {
        std::string text = mutate(kTemplateSeeds[rng.below(kTemplateSeeds.size())], rng, kTemplateTokens);
        checkTemplate(text);
        total++;
        parsed += parseFragment(text).ok();
    }
    MESSAGE(parsed << " of " << total << " mutated templates parsed");
}

TEST_CASE("fuzz: mutated Markdown") {
    Rng rng{0x5EEDu};
    for (const std::string& seed : kMarkdownSeeds) checkMarkdown(seed);
    for (int i = 0; i < 20000; i++) {
        checkMarkdown(mutate(kMarkdownSeeds[rng.below(kMarkdownSeeds.size())], rng, kMarkdownTokens));
    }
    // Pure noise too.
    for (int i = 0; i < 2000; i++) {
        std::string noise;
        size_t n = rng.below(200);
        for (size_t k = 0; k < n; k++) noise += static_cast<char>(rng.below(256));
        checkMarkdown(noise);
    }
}

TEST_CASE("fuzz: the template parser is linear on adversarial input") {
    auto parse = [](const std::string& text) { parseFragment(text); };
    const size_t n = 4000;
    checkLinear("{{", [](size_t k) { return repeat("{{", k); }, parse, n);
    checkLinear("<a ", [](size_t k) { return repeat("<a ", k); }, parse, n);
    checkLinear("attributes", [](size_t k) { return "<a " + repeat("x=\"{{y}}\" ", k) + ">"; }, parse, n);
    checkLinear("comments", [](size_t k) { return repeat("<!-- -- ", k) + "-->"; }, parse, n);
    checkLinear("fields", [](size_t k) { return repeat("<p>{{a.b.c}}</p>", k); }, parse, n);
    checkLinear("script slashes", [](size_t k) { return "<script>" + repeat(")/", k) + "</script>"; }, parse, n);
    checkLinear("script forks", [](size_t k) { return "<script>" + repeat("(a)/b/ {{x}}\n", k) + "</script>"; },
                parse, n / 4);
    checkLinear("script strings", [](size_t k) { return "<script>" + repeat("'a\\'b' + {{x}};", k) + "</script>"; },
                parse, n / 4);
    checkLinear("svg", [](size_t k) { return repeat("<svg>", k) + repeat("</svg>", k); }, parse, n);
    checkLinear("includes", [](size_t k) { return repeat("{{> a}}", k); }, parse, n);
}

TEST_CASE("fuzz: rendering is linear") {
    ParsedTemplate p = parseFragment("<li title=\"{{a}}\"><a href=\"{{u}}\">{{a}}</a></li>");
    REQUIRE(p.ok());
    auto run = [&](const std::string& value) {
        MapFields f;
        f.set("a", FieldValue::string(value));
        f.set("u", FieldValue::string(value));
        render(p.compiled, f);
    };
    checkLinear("escaping", [](size_t k) { return repeat("<\"'&", k); }, run, 50000);
    checkLinear("URLs", [](size_t k) { return "/" + repeat("a b%zz", k); }, run, 50000);
}

TEST_CASE("fuzz: Markdown is linear on adversarial input") {
    auto run = [](const std::string& text) {
        MarkdownOptions o;
        o.profile = MarkdownProfile::Full;
        renderMarkdown(text, o);
    };
    const size_t n = 5000;
    checkLinear("[", [](size_t k) { return repeat("[", k) + "a"; }, run, n);
    checkLinear("[a](", [](size_t k) { return repeat("[a](", k); }, run, n);
    checkLinear("![", [](size_t k) { return repeat("![", k) + "a"; }, run, n);
    checkLinear("*", [](size_t k) { return repeat("*", k) + "a"; }, run, n);
    checkLinear("*a", [](size_t k) { return repeat("*a", k); }, run, n);
    checkLinear("**_", [](size_t k) { return repeat("**_a", k); }, run, n);
    checkLinear("_", [](size_t k) { return repeat("_a ", k); }, run, n);
    checkLinear("`", [](size_t k) { return repeat("`a", k); }, run, n);
    checkLinear("<", [](size_t k) { return repeat("<", k) + "a"; }, run, n);
    checkLinear("<a", [](size_t k) { return repeat("<a ", k); }, run, n);
    checkLinear("&", [](size_t k) { return repeat("&#", k); }, run, n);
    checkLinear("> nesting", [](size_t k) { return repeat(">", k) + " a"; }, run, n);
    checkLinear("lists", [](size_t k) { return repeat("- a\n  ", k); }, run, n);
    checkLinear("links", [](size_t k) { return repeat("[a]", k) + "\n\n[a]: /x"; }, run, n);
    checkLinear("reference defs", [](size_t k) { return repeat("[a]: /x\n", k); }, run, n);
    checkLinear("autolinks", [](size_t k) { return repeat("www.a.", k); }, run, n);
    checkLinear("table columns", [](size_t k) {
        return repeat("| a ", k) + "|\n" + repeat("|---", k) + "|\n" + repeat("| b ", k) + "|\n";
    }, run, n / 4);
    checkLinear("table rows", [](size_t k) { return "| a | b |\n|---|---|\n" + repeat("| 1 | 2 |\n", k); }, run, n);
}

TEST_CASE("fuzz: the Markdown writer") {
    Rng rng{0xA11CEu};
    for (const std::string& seed : kMarkdownSeeds) checkWriter(seed);
    for (int i = 0; i < 10000; i++) {
        checkWriter(mutate(kMarkdownSeeds[rng.below(kMarkdownSeeds.size())], rng, kMarkdownTokens));
    }
    for (int i = 0; i < 1000; i++) {
        std::string noise;
        size_t n = rng.below(200);
        for (size_t k = 0; k < n; k++) noise += static_cast<char>(rng.below(256));
        checkWriter(noise);
    }
}

TEST_CASE("fuzz: mutated Markdown templates") {
    Rng rng{0xD1A10u};
    int parsed = 0, total = 0;
    for (const std::string& seed : kMarkdownTemplateSeeds) checkMarkdownTemplate(seed);
    for (int i = 0; i < 5000; i++) {
        std::string text =
            mutate(kMarkdownTemplateSeeds[rng.below(kMarkdownTemplateSeeds.size())], rng, kMarkdownTemplateTokens);
        checkMarkdownTemplate(text);
        total++;
        parsed += parseMarkdownTemplate(codeFile("frags/m.md", text), TemplateKind::Fragment).ok();
    }
    MESSAGE(parsed << " of " << total << " mutated Markdown templates parsed");
}

TEST_CASE("fuzz: the Markdown writer and Markdown templates are linear") {
    auto write = [](const std::string& text) { writeMarkdown(text, MarkdownProfile::Full); };
    const size_t n = 5000;
    checkLinear("write [", [](size_t k) { return repeat("[", k) + "a"; }, write, n);
    checkLinear("write *a", [](size_t k) { return repeat("*a", k); }, write, n);
    checkLinear("write **_", [](size_t k) { return repeat("**_a", k); }, write, n);
    checkLinear("write `", [](size_t k) { return repeat("`a", k); }, write, n);
    checkLinear("write <", [](size_t k) { return repeat("<a ", k); }, write, n);
    checkLinear("write > nesting", [](size_t k) { return repeat(">", k) + " a"; }, write, n);
    checkLinear("write lists", [](size_t k) { return repeat("- a\n  ", k); }, write, n);
    checkLinear("write digits", [](size_t k) { return repeat("1", k) + ". a"; }, write, n);
    checkLinear("write www", [](size_t k) { return repeat("www.", k); }, write, n);
    checkLinear("write code fences", [](size_t k) { return "```\n" + repeat("`", k) + "\n```"; }, write, n);
    checkLinear("write table", [](size_t k) { return "| a | b |\n|---|---|\n" + repeat("| 1 | 2 |\n", k); }, write,
                n);

    auto parse = [](const std::string& text) {
        parseMarkdownTemplate(codeFile("frags/m.md", text), TemplateKind::Fragment);
    };
    checkLinear("template fields", [](size_t k) { return repeat("{{a}} ", k); }, parse, n);
    checkLinear("template {{", [](size_t k) { return repeat("{{", k); }, parse, n);
    checkLinear("template [[", [](size_t k) { return repeat("\\[[", k); }, parse, n);
    checkLinear("template brackets", [](size_t k) { return repeat("[", k) + "{{a}}"; }, parse, n);
    checkLinear("template links", [](size_t k) { return repeat("[a](/x?{{a}}) ", k); }, parse, n);
    checkLinear("template link lines", [](size_t k) { return "x" + repeat(" ", k) + "](/{{a}})"; }, parse, n);
    checkLinear("template runs", [](size_t k) { return repeat("{{a}}x", k); }, parse, n / 4);
    checkLinear("template blocks", [](size_t k) { return repeat("{{a}}\n\n", k); }, parse, n);
}

TEST_CASE("fuzz: form bodies are read or refused, within limits, in linear time") {
    Rng rng{0x464F524D53ull};
    const std::vector<std::string> tokens = {
        "--B", "--B--", "\r\n", "\r\n\r\n", "Content-Disposition: form-data; name=\"", "\"; filename=\"",
        "\"", ";", "=", "%", "%2", "%FF", "&", "+", "_csrf", "_t", "_pow", "\xC3", "\xFF", std::string(1, '\0'),
        "a", "who", "\\", "%22", "%0A", "name=", "filename*=", "--", "\x89PNG\r\n\x1a\n"};
    const std::string seed =
        "--B\r\nContent-Disposition: form-data; name=\"who\"\r\n\r\nAda\r\n"
        "--B\r\nContent-Disposition: form-data; name=\"pic\"; filename=\"a.png\"\r\nContent-Type: image/png\r\n\r\n"
        "\x89PNG\r\n\x1a\nxx\r\n--B--\r\n";
    FormSpec spec;
    spec.name = "f";
    FormField who;
    who.name = "who";
    who.required = true;
    who.max = 5;
    FormField pic;
    pic.name = "pic";
    pic.type = FormFieldType::File;
    pic.maxKB = 1;
    pic.types = {"image/png"};
    FormField pick;
    pick.name = "a";
    pick.type = FormFieldType::Choices;
    pick.options = {"x", "y"};
    spec.fields = {who, pic, pick};
    SubmissionLimits limits;
    limits.maxParts = 20;
    limits.maxFields = 50;
    limits.maxFormBytes = 4096;
    for (int i = 0; i < 4000; i++) {
        HttpRequest r;
        r.method = "POST";
        bool multipart = i % 2 == 0;
        r.headers.add("Content-Type", multipart ? "multipart/form-data; boundary=B" : "application/x-www-form-urlencoded");
        r.body = mutate(multipart ? seed : "who=Ada&a=x&_csrf=C", rng, tokens);
        SubmissionError error;
        std::optional<Submission> s = readSubmission(r, limits, error);
        if (!s) {
            CHECK((error.status == 400 || error.status == 413));
            continue;
        }
        CHECK_LE(s->fields.size(), multipart ? limits.maxParts : limits.maxFields);
        for (const auto& [name, value] : s->fields) {
            CHECK(isValidUtf8(name));
            CHECK(isValidUtf8(value));
        }
        CheckedForm c = checkForm(spec, *s);
        CHECK(c.form.kind() == Datum::Kind::Map);
    }

    // Linear time: many tiny parts, and a body of nothing but delimiters.
    auto timed = [&](const std::string& body, const char* type) {
        HttpRequest r;
        r.method = "POST";
        r.headers.add("Content-Type", type);
        r.body = body;
        SubmissionLimits big;
        big.maxParts = 1u << 20;
        big.maxFields = 1u << 20;
        big.maxFormBytes = 64u << 20;
        auto start = std::chrono::steady_clock::now();
        SubmissionError error;
        (void)readSubmission(r, big, error);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    std::string parts, delimiters, pairs;
    for (int i = 0; i < 20000; i++) parts += "--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nx\r\n";
    parts += "--B--\r\n";
    for (int i = 0; i < 200000; i++) delimiters += "--B";
    for (int i = 0; i < 100000; i++) pairs += "a=b&";
    CHECK_LT(timed(parts, "multipart/form-data; boundary=B"), 2.0);
    CHECK_LT(timed(delimiters, "multipart/form-data; boundary=B"), 2.0);
    CHECK_LT(timed(pairs, "application/x-www-form-urlencoded"), 2.0);
}

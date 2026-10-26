// Fuzzing the template tokenizer and the Markdown renderer, deterministically
// (a fixed seed), so it can run with every test run. See CMakeLists.txt.
//
// - Mutated templates must parse or be refused without crashing, and when
//   they render, a hostile value must never come out unescaped.
// - Mutated Markdown must only ever produce the tags and attributes the
//   renderer writes, with safe URLs.
// - Adversarial inputs must take time linear in their size.

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../template/helpers.h"
#include "doctest.h"
#include "markdown/markdown.h"

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
                if (attr == "href" || attr == "src") {
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
    for (auto profile : {MarkdownProfile::Comment, MarkdownProfile::Full}) {
        MarkdownOptions options;
        options.profile = profile;
        MarkdownResult r = renderMarkdown(text, options);
        REQUIRE(r.ok());
        checkMarkdownOutput(r.html.str(), text);
        if (profile == MarkdownProfile::Comment) {
            CHECK(r.html.str().find("<img") == std::string::npos);
            CHECK(r.html.str().find(" id=") == std::string::npos);
            for (const char* h : {"<h1", "<h2", "<h3", "<h4", "<h5", "<h6"}) {
                CHECK(r.html.str().find(h) == std::string::npos);
            }
        }
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

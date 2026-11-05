// The forms and limits passes (overview §4, Forms and validation; §5, Rate
// limits and spam): declarations read at compile time, checked, and matched
// against the templates' forms.
#include "compiler/compiler.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

CompileOutput compileApp(const std::string& root) {
    auto app = AppFolder::open(root, AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto tree = app.value().codeTree();
    REQUIRE(tree.ok());
    auto host = ScriptHost::create();
    REQUIRE(host.ok());
    CompileOptions options;
    options.scripts = host.value().get();
    options.evaluator = host.value().get();
    return compile(tree.value(), options);
}

std::string allDiagnostics(const CompileOutput& out) {
    std::string s;
    for (const Diagnostic& d : out.diagnostics) s += d.toString() + "\n";
    return s;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The HTML a template renders with no fields.
std::string rendered(const CompiledTemplate& t, RenderOptions options = {}) {
    options.warnings = false;
    return render(t, EmptyFields(), options).html.str();
}

const char* const kBook =
    "form.sign = {\n"
    "    \"who\":   {\"type\": \"text\", \"required\": true, \"max\": 40},\n"
    "    \"msg\":   {\"type\": \"multiline\", \"required\": true, \"min\": 2, \"max\": 2000, \"maxLinks\": 2},\n"
    "    \"age\":   {\"type\": \"int\", \"min\": 0, \"max\": 150},\n"
    "    \"agree\": {\"type\": \"bool\", \"required\": true},\n"
    "    \"mood\":  {\"type\": \"choice\", \"options\": [\"happy\", \"meh\"], \"required\": true},\n"
    "    \"photo\": {\"type\": \"file\", \"maxKB\": 64, \"types\": [\"image/png\"]},\n"
    "}\n"
    "render = function(req)\n"
    "    return {\"form\": frag(\"signForm\", formState(\"sign\"))}\n"
    "end function\n"
    "post.sign = function(req)\n"
    "    if req.errors then return reshow(\"signForm\")\n"
    "    return redirect(\"/book\")\n"
    "end function\n";

}  // namespace

TEST_CASE("forms: a declaration becomes the post handler's FormSpec") {
    TempDir dir;
    dir.write("code/pages/book.html", "<!DOCTYPE html><html><body>{{form}}</body></html>");
    dir.write("code/pages/book.ms", kBook);
    dir.write("code/frags/signForm.html",
              "<form id=\"signForm\" method=\"post\" action=\"/book/sign\" enctype=\"multipart/form-data\">\n"
              "<input name=\"who\" value=\"{{who}}\" maxlength=\"20\">\n"
              "<textarea name=\"msg\">{{msg}}</textarea>\n"
              "<input type=\"number\" name=\"age\" value=\"{{age}}\">\n"
              "<input type=\"checkbox\" name=\"agree\" checked=\"{{agree}}\">\n"
              "<select name=\"mood\"><option>happy</option><option>meh</option></select>\n"
              "<input type=\"file\" name=\"photo\">\n"
              "<input name=\"nickname\">\n"
              "<button name=\"go\" value=\"1\">Sign</button>\n"
              "</form>");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const RouteEntry* sign = out.app->routes.find("/book/sign");
    REQUIRE(sign);
    REQUIRE(sign->form);
    CHECK_EQ(sign->form->fields.size(), 6u);
    CHECK_EQ(sign->pageUrl, "/book");
    CHECK(sign->usesSession);   // reshow keeps the input in the session
    const FormField* msg = sign->form->find("msg");
    REQUIRE(msg);
    CHECK(msg->type == FormFieldType::Multiline);
    CHECK_EQ(*msg->maxLinks, 2);
    CHECK_EQ(sign->form->maxUploadBytes(), 64u * 1024);
    CHECK(out.app->forms.count("pages/book.ms#sign"));
    // The page that calls formState uses the session too.
    CHECK(out.app->routes.find("/book")->usesSession);

    // Inputs it doesn't declare, and fields with no input, are warnings.
    std::string d = allDiagnostics(out);
    CHECK(contains(d, "code/frags/signForm.html:8: warning: <input name=\"nickname\"> posts to /book/sign, whose "
                      "form.sign (code/pages/book.ms) doesn't declare it, so it would be dropped"));
    CHECK(contains(d, "<button name=\"go\">"));

    // Browser-side validation attributes, where the markup has none.
    std::string html = rendered(*out.app->templates.fragment("signForm"));
    // (A value attribute that's one field is left out when the field is empty.)
    CHECK(contains(html, "<input name=\"who\" maxlength=\"20\" required>"));   // its own maxlength kept
    CHECK(contains(html, "<textarea name=\"msg\" required maxlength=\"2000\" minlength=\"2\">"));
    CHECK(contains(html, "<input type=\"number\" name=\"age\" min=\"0\" max=\"150\" step=\"1\">"));
    CHECK(contains(html, "<input type=\"checkbox\" name=\"agree\" required>"));
    CHECK(contains(html, "<select name=\"mood\" required>"));
    CHECK(contains(html, "<input type=\"file\" name=\"photo\" accept=\"image/png\">"));
    CHECK(contains(html, "<input name=\"nickname\">"));
    CHECK_EQ(out.app->templates.fragment("signForm")->rootId(), "signForm");
}

TEST_CASE("forms: a declared field with no input is a warning") {
    TempDir dir;
    dir.write("code/pages/book.html",
              "<!DOCTYPE html><html><body><form method=\"post\" action=\"book/sign\"><input name=\"who\">"
              "</form></body></html>");
    dir.write("code/pages/book.ms",
              "form.sign = {\"who\": {\"type\": \"text\"}, \"msg\": {\"type\": \"multiline\"}}\n"
              "post.sign = function(req)\nend function\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    CHECK(contains(allDiagnostics(out), "code/pages/book.html:1: warning: this form posts to /book/sign, whose "
                                        "form.sign declares \"msg\", but it has no input named \"msg\""));
}

TEST_CASE("forms: mistakes in a declaration are errors") {
    struct Case {
        const char* declaration;
        const char* expected;
    };
    const Case cases[] = {
        {"form.nope = {\"a\": {\"type\": \"text\"}}", "form.nope declares a form for post.nope, but there's no post.nope"},
        {"form.sign = \"who\"", "form.sign must be a map from field names"},
        {"form.sign = {}", "form.sign must be a map from field names"},
        {"form.sign = {\"errors\": {\"type\": \"text\"}}", "a field can't be named \"errors\""},
        {"form.sign = {\"_csrf\": {\"type\": \"text\"}}", "\"_csrf\" can't be a field name"},
        {"form.sign = {\"a\": {}}", "form.sign.a needs a \"type\""},
        {"form.sign = {\"a\": {\"type\": \"color\"}}", "\"color\" isn't a field type"},
        {"form.sign = {\"a\": {\"type\": \"text\", \"maxlength\": 3}}", "maxlength isn't a field setting"},
        {"form.sign = {\"a\": {\"type\": \"bool\", \"max\": 3}}", "max applies to text"},
        {"form.sign = {\"a\": {\"type\": \"choice\"}}", "options is needed"},
        {"form.sign = {\"a\": {\"type\": \"choice\", \"options\": [\"x\", \"x\"]}}", "options has \"x\" twice"},
        {"form.sign = {\"a\": {\"type\": \"file\", \"maxKB\": 10}}", "types is needed for a file field"},
        {"form.sign = {\"a\": {\"type\": \"file\", \"types\": [\"image/svg+xml\"]}}", "can't be checked by content"},
        {"form.sign = {\"a\": {\"type\": \"file\", \"types\": [\"image/png\"], \"maxKB\": 99999}}", "at most 16384"},
        {"form.sign = {\"a\": {\"type\": \"text\", \"min\": 5, \"max\": 2}}", "min is more than max"},
        {"form.sign = {\"a\": {\"type\": \"text\", \"messages\": {\"tooLong\": \"x\"}}}", "\"tooLong\", which isn't a rule"},
        {"form.sign = {\"a\": {\"type\": \"text\", \"required\": \"yes\"}}", "required must be true or false"},
        {"form = 3", "form must stay a map"},
        {"form.sign = {\"a\": {\"type\": \"text\", \"f\": @print}}", "form.sign must be a map of fields; it contains a function"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.declaration);
        TempDir dir;
        dir.write("code/pages/x.ms", std::string(c.declaration) + "\npost.sign = function(req)\nend function\n");
        CompileOutput out = compileApp(dir.path());
        CHECK_FALSE(out.ok());
        CHECK_MESSAGE(contains(allDiagnostics(out), c.expected), allDiagnostics(out));
    }
}

TEST_CASE("limits: rules from code/limits.ms, combined per route") {
    TempDir dir;
    dir.write("code/pages/guestbook.ms",
              "post.sign = function(req)\nend function\nget.more = function(req)\nend function\n");
    dir.write("code/pages/api/users.ms", "get.list = function(req)\nend function\n");
    dir.write("code/pages/index.html", "<!DOCTYPE html><html><body>"
                                       "<form method=\"post\" action=\"/guestbook/sign\"><input name=x></form>"
                                       "<form hx-post=\"/api/users/list\"><input name=y></form></body></html>");
    dir.write("code/limits.ms",
              "limits = {}\n"
              "limits[\"/guestbook\"] = \"100/hour\"\n"
              "limits[\"/guestbook/sign\"] = {\"rate\": [\"5/minute\", \"50/day\"], \"minSeconds\": 3, "
              "\"challenge\": true, \"maxAgeMinutes\": 60}\n"
              "limits[\"/api\"] = \"100/minute per user\"\n"
              "limits[\"/gone\"] = \"1/second\"\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const RouteEntry* sign = out.app->routes.find("/guestbook/sign");
    REQUIRE(sign);
    REQUIRE_EQ(sign->limits.size(), 3u);   // the folder's rule, and its own two
    CHECK_EQ(sign->limits[0].describe(), "100/hour per ip");
    CHECK_EQ(sign->limits[0].key, "code/limits.ms: /guestbook");
    CHECK_EQ(sign->limits[1].describe(), "5/minute per ip");
    CHECK_EQ(sign->limits[1].key, "code/limits.ms: /guestbook/sign");
    CHECK_EQ(sign->minSeconds, 3);
    CHECK_EQ(sign->maxAgeSeconds, 3600);
    CHECK(sign->challenge);
    const RouteEntry* more = out.app->routes.find("/guestbook/more");
    REQUIRE_EQ(more->limits.size(), 1u);
    CHECK_FALSE(more->challenge);
    CHECK(out.app->routes.find("/api/users/list")->limits[0].per == RateRule::Per::User);
    CHECK(out.app->routes.find("/")->limits.empty());
    CHECK(contains(allDiagnostics(out), "code/limits.ms: warning: the rule for /gone covers no page"));

    // Forms posting to a challenge route carry a puzzle, and the page links
    // the solver; others don't.
    RenderOptions options;
    options.newChallenge = [] { return std::string("PUZZLE"); };
    options.challengeScript = "/__fourfold/challenge.js";
    options.formTimeToken = "T";
    std::string html = rendered(*out.app->templates.find(RelPath::parse("pages/index.html").value()), options);
    CHECK(contains(html, "<input type=\"hidden\" name=\"_pow\" value=\"\" data-ff-challenge=\"PUZZLE\">"));
    size_t puzzles = 0;
    for (size_t at = html.find("_pow"); at != std::string::npos; at = html.find("_pow", at + 1)) puzzles++;
    CHECK_EQ(puzzles, 1u);
    CHECK(contains(html, "<script src=\"/__fourfold/challenge.js\" defer></script>"));
    CHECK(contains(html, "<input type=\"hidden\" name=\"_t\" value=\"T\">"));
}

TEST_CASE("limits: mistakes are errors") {
    struct Case {
        const char* rule;
        const char* expected;
    };
    const Case cases[] = {
        {"limits[\"/x\"] = \"5 a minute\"", "isn't a rate"},
        {"limits[\"/x\"] = \"5/fortnight\"", "the unit must be"},
        {"limits[\"/x\"] = []", "a rate is a string"},
        {"limits[\"/x\"] = {\"rate\": \"5/minute\", \"burst\": 3}", "unknown setting \"burst\""},
        {"limits[\"/x\"] = {\"minSeconds\": -1}", "minSeconds must be"},
        {"limits[\"/x\"] = {\"maxAgeMinutes\": 5}", "maxAgeMinutes needs minSeconds"},
        {"limits[\"/x\"] = {\"challenge\": \"yes\"}", "challenge must be true or false"},
        {"limits[\"/x/*\"] = \"5/minute\"", "no wildcards"},
        {"limits[\"/x/\"] = \"5/minute\"", "without the trailing slash"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.rule);
        TempDir dir;
        dir.write("code/pages/x.html", "<!DOCTYPE html>x");
        dir.write("code/limits.ms", std::string("limits = {}\n") + c.rule + "\n");
        CompileOutput out = compileApp(dir.path());
        CHECK_FALSE(out.ok());
        CHECK_MESSAGE(contains(allDiagnostics(out), c.expected), allDiagnostics(out));
    }
    TempDir dir;
    dir.write("code/pages/x.html", "<!DOCTYPE html>x");
    dir.write("code/limits.ms", "rules = {}\n");
    CHECK(contains(allDiagnostics(compileApp(dir.path())), "must set `limits`"));
}

TEST_CASE("pages/429.html is the too-many-requests page, not a route") {
    TempDir dir;
    dir.write("code/pages/429.html", "<!DOCTYPE html><html><body>Slow down</body></html>");
    CompileOutput out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK_FALSE(out.app->routes.find("/429"));
    REQUIRE(out.app->tooManyPage);
    CHECK_EQ(out.app->tooManyPage->str(), "pages/429.html");
}

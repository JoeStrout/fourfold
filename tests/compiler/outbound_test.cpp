// The outbound pass, webhooks and mail templates (M9; overview §6).
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

}  // namespace

TEST_CASE("outbound.ms: the allowlist and mailTo") {
    TempDir dir;
    dir.write("code/pages/index.html", "<!DOCTYPE html><html><body>hi</body></html>");
    CompileOutput none = compileApp(dir.path());
    REQUIRE(none.ok());
    CHECK(!none.app->outbound.declared);   // no file: nothing allowed

    dir.write("code/outbound.ms", "allow = [\"api.weather.gov\", \"*.stripe.com\", \"http://feeds.example.org\"]\n"
                                  "mailTo = [\"Owner@Example.com\", \"*@staff.example.com\"]\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const OutboundPolicy& p = out.app->outbound;
    CHECK(p.declared);
    CHECK(p.allowsHost("api.weather.gov", 443, true));
    CHECK(p.allowsHost("api.stripe.com", 443, true));
    CHECK(p.allowsHost("feeds.example.org", 80, false));
    CHECK(!p.allowsHost("api.weather.gov", 80, false));
    CHECK(p.allowsMailTo("owner@example.com"));
    CHECK(p.allowsMailTo("bo@staff.example.com"));
    CHECK(!p.allowsMailTo("bo@example.com"));
    CHECK(out.diagnostics.empty());

    // "*" is allowed, with a warning.
    dir.write("code/outbound.ms", "allow = [\"*\"]\nmailTo = [\"*\"]\n");
    out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(out.app->outbound.anyHost());
    CHECK(contains(allDiagnostics(out), "any public host"));
    CHECK(contains(allDiagnostics(out), "any address"));

    struct Bad {
        const char* source;
        const char* message;
    };
    for (const Bad& b : std::vector<Bad>{
             {"allow = \"api.example.com\"\n", "list of strings"},
             {"allow = [\"https://api.example.com/v1\"]\n", "no path"},
             {"allow = [\"*.com\"]\n", "top-level domain"},
             {"allow = [\"api.*.com\"]\n", "a * can only"},
             {"allow = [\"api.example.com:99999\"]\n", "port"},
             {"mailTo = [\"not an address\"]\n", "email address"},
             {"allow = [1]\n", "list of strings"},
         }) {
        dir.write("code/outbound.ms", b.source);
        out = compileApp(dir.path());
        CHECK_MESSAGE(!out.ok(), b.source);
        CHECK_MESSAGE(contains(allDiagnostics(out), b.message), b.source, allDiagnostics(out));
    }
    dir.write("code/outbound.ms", "x = 1\n");
    out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(contains(allDiagnostics(out), "allows nothing"));
}

TEST_CASE("webhooks: routed for POST, never beside a post handler of the same name") {
    TempDir dir;
    dir.write("code/pages/pay.ms",
              "webhook.stripe = function(req)\n    return null\nend function\n"
              "get.stripe = function(req)\n    return \"hi\"\nend function\n"
              "post.thanks = function(req)\n    return null\nend function\n");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    const RouteEntry* stripe = out.app->routes.find("/pay/stripe");
    REQUIRE(stripe);
    REQUIRE(stripe->webhook);
    CHECK(stripe->webhook->label() == "webhook.stripe");
    CHECK(stripe->get);
    CHECK(!stripe->post);
    CHECK(contains(describe(*out.app), "webhook.stripe in pages/pay.ms"));

    dir.write("code/pages/pay.ms",
              "webhook.stripe = function(req)\n    return null\nend function\n"
              "post.stripe = function(req)\n    return null\nend function\n");
    out = compileApp(dir.path());
    CHECK(!out.ok());
    CHECK(contains(allDiagnostics(out), "can't both be defined"));

    // A limits rule's timing check and challenge can't apply to a webhook.
    dir.write("code/pages/pay.ms", "webhook.stripe = function(req)\n    return null\nend function\n");
    dir.write("code/limits.ms", "limits = {}\nlimits[\"/pay\"] = {\"rate\": \"100/minute\", \"minSeconds\": 2}\n");
    out = compileApp(dir.path());
    REQUIRE(out.ok());
    CHECK(contains(allDiagnostics(out), "a webhook's caller isn't a browser"));
}

TEST_CASE("mail templates: code/mail/*.md") {
    TempDir dir;
    dir.write("code/frags/sig.md", "-- The team\n");
    dir.write("code/mail/welcome.md", "# Welcome, {{name}}!\n\n{{> sig}}\n");
    dir.write("code/mail/notes/reminder.md", "Don't forget: {{what}}\n");
    dir.write("code/mail/readme.txt", "not a template");
    CompileOutput out = compileApp(dir.path());
    REQUIRE_MESSAGE(out.ok(), allDiagnostics(out));
    CHECK(out.app->mailTemplates.count("welcome"));
    CHECK(out.app->mailTemplates.count("notes/reminder"));
    CHECK(out.app->mailTemplates.size() == 2);
    CHECK(contains(allDiagnostics(out), "only .md files are mail templates"));

    dir.write("code/mail/broken.md", "Hi {{> nosuchfrag}}\n");
    out = compileApp(dir.path());
    CHECK(!out.ok());
}

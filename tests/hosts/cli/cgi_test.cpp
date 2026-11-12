// `fourfold cgi`, `fourfold run-jobs`, and the web root check (M10; overview
// §8, Hosting modes).
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cgi.h"
#include "cli.h"
#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string cgi(const TempDir& dir, CgiVariables vars, const std::string& body = "") {
    if (!vars.count("REQUEST_METHOD")) vars["REQUEST_METHOD"] = "GET";
    vars["HTTP_HOST"] = "example.com";
    vars["REMOTE_ADDR"] = "127.0.0.1";
    if (!body.empty()) vars["CONTENT_LENGTH"] = std::to_string(body.size());
    std::istringstream in(body);
    std::ostringstream out, err;
    cli::runCgi(dir.at("app"), vars, in, out, err, false);
    return out.str();
}

}  // namespace

TEST_CASE("fourfold cgi: serves a request, and refuses an app inside the web root") {
    TempDir dir;
    AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>hello</body></html>");
    std::string response = cgi(dir, {{"REQUEST_URI", "/"}, {"DOCUMENT_ROOT", dir.at("public_html")}});
    CHECK(response.rfind("Status: 200 OK\r\n", 0) == 0);
    CHECK(contains(response, "hello"));

    std::filesystem::create_directories(dir.at("www"));
    response = cgi(dir, {{"REQUEST_URI", "/"}, {"DOCUMENT_ROOT", dir.path()}});
    CHECK(response.rfind("Status: 500", 0) == 0);
    CHECK_FALSE(contains(response, "hello"));

    std::string big(2 * 1024 * 1024, 'x');
    response = cgi(dir, {{"REQUEST_URI", "/"}, {"REQUEST_METHOD", "POST"}}, big);
    CHECK(response.rfind("Status: 413", 0) == 0);
}

TEST_CASE("webRootProblem: the app's folders must be out of the web server's reach") {
    TempDir dir;
    for (const char* f : {"home/app/code", "home/app/data", "home/app/private", "home/public_html", "home/app/www"}) {
        std::filesystem::create_directories(dir.at(f));
    }
    std::string app = std::filesystem::canonical(dir.at("home/app")).string();
    CHECK(cli::webRootProblem(app, dir.at("home/public_html")).empty());
    CHECK(cli::webRootProblem(app, dir.at("home/app/www")).empty());   // a folder of its own, beside the partitions
    CHECK(cli::webRootProblem(app, dir.at("missing")).empty());
    CHECK(contains(cli::webRootProblem(app, dir.at("home")), "inside the web root"));
    CHECK(contains(cli::webRootProblem(app, dir.at("home/app")), "the app folder itself"));
    CHECK(contains(cli::webRootProblem(app, dir.at("home/app/code")), "code/"));
    CHECK(contains(cli::webRootProblem(app, dir.at("home/app/private")), "private/"));
}

TEST_CASE("fourfold run-jobs: runs what CGI requests enqueued, and what's scheduled") {
    TempDir dir;
    AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>hi</body></html>");
    dir.write("app/code/pages/work.ms", "get.queue = function(req)\n    enqueue \"note\", {\"text\": req.query.t}\n"
                                        "    return \"queued\"\nend function\n");
    dir.write("app/code/jobs/notes.ms", "job.note = function(args)\n    data.set \"note-\" + args.text, 1\n"
                                        "end function\n"
                                        "job.tick = function\n    data.set \"ticked\", 1\nend function\n");
    dir.write("app/code/schedule.ms", "schedule = {}\nschedule[\"tick\"] = \"every 1h\"\n");
    CHECK(contains(cgi(dir, {{"REQUEST_URI", "/work/queue?t=one"}}), "queued"));
    CHECK(contains(cgi(dir, {{"REQUEST_URI", "/work/queue?t=two"}}), "queued"));
    CHECK_FALSE(std::filesystem::exists(dir.at("app/data/note-one.json")));

    std::ostringstream out, err;
    CHECK(cli::run({"run-jobs", dir.at("app")}, out, err) == 0);
    CHECK_MESSAGE(err.str().empty(), err.str());
    CHECK(std::filesystem::exists(dir.at("app/data/note-one.json")));
    CHECK(std::filesystem::exists(dir.at("app/data/note-two.json")));
    CHECK(std::filesystem::exists(dir.at("app/data/ticked.json")));

    // Nothing due: it returns at once.
    auto start = std::chrono::steady_clock::now();
    CHECK(cli::run({"run-jobs", dir.at("app")}, out, err) == 0);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(cli::run({"run-jobs"}, out, err) == 2);
}

TEST_CASE("fourfold cgi-setup: the entry point and .htaccess, outside the app") {
    TempDir dir;
    std::ostringstream out, err;
    std::string binary = std::filesystem::canonical("/bin/sh").string();   // any program will do
    CHECK(cli::run({"cgi-setup", dir.at("app"), dir.at("public_html"), "--binary", binary}, out, err) == 0);
    CHECK_MESSAGE(err.str().empty(), err.str());
    std::string app = std::filesystem::canonical(dir.at("app")).string();
    CHECK(std::filesystem::exists(dir.at("app/code")));
    std::ifstream script(dir.at("public_html/fourfold.cgi"));
    std::string text((std::istreambuf_iterator<char>(script)), std::istreambuf_iterator<char>());
    CHECK(contains(text, "exec '" + binary + "' cgi '" + app + "'"));
    auto perms = std::filesystem::status(dir.at("public_html/fourfold.cgi")).permissions();
    CHECK((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
    CHECK((perms & std::filesystem::perms::group_write) == std::filesystem::perms::none);
    CHECK((perms & std::filesystem::perms::others_write) == std::filesystem::perms::none);
    std::ifstream ht(dir.at("public_html/.htaccess"));
    std::string htaccess((std::istreambuf_iterator<char>(ht)), std::istreambuf_iterator<char>());
    CHECK(contains(htaccess, "RewriteRule ^ fourfold.cgi [L"));
    CHECK(contains(out.str(), "run-jobs"));

    // Again, for FastCGI: replaces its own files.
    CHECK(cli::run({"cgi-setup", dir.at("app"), dir.at("public_html"), "--binary", binary, "--fastcgi"}, out, err) == 0);
    CHECK(std::filesystem::exists(dir.at("public_html/fourfold.fcgi")));
    CHECK_FALSE(std::filesystem::exists(dir.at("public_html/fourfold.cgi")));

    // Someone else's .htaccess is left alone.
    dir.write("site2/.htaccess", "Redirect /old /new\n");
    std::ostringstream err2;
    CHECK(cli::run({"cgi-setup", dir.at("app"), dir.at("site2"), "--binary", binary}, out, err2) == 1);
    CHECK(contains(err2.str(), "wasn't written by cgi-setup"));

    // An app inside the web root is refused.
    std::ostringstream err3;
    CHECK(cli::run({"cgi-setup", dir.at("www/app"), dir.at("www"), "--binary", binary}, out, err3) == 1);
    CHECK(contains(err3.str(), "inside the web root"));
}

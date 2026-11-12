// CGI mode, end to end through CgiApp (M10; overview §8, Hosting modes).
// Each CgiApp stands for a CGI process: they share only the app folder.
#include "cgi/cgi_app.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string logText(const TempDir& dir) {
    std::string all;
    for (const auto& e : std::filesystem::directory_iterator(dir.at("app/private/state/logs"))) all += slurp(e.path());
    return all;
}

struct Site {
    TempDir dir;
    ManualClock clock;

    Site() { AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value(); }

    // One "process": open, serve one request, close.
    HttpResponse request(const std::string& target, const std::string& cookie = "") {
        CgiOptions options;
        options.clock = &clock;
        auto app = CgiApp::open(dir.at("app"), options);
        REQUIRE_MESSAGE(app.ok(), (app.ok() ? "" : app.error().message));
        CgiVariables vars{{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", target}, {"HTTP_HOST", "example.com"},
                          {"REMOTE_ADDR", "203.0.113.5"}};
        if (!cookie.empty()) vars["HTTP_COOKIE"] = cookie;
        return app.value()->handle(cgiRequest(vars, ""), false);
    }

    void deploy() {
        dir.write("app/code/.deploy", std::to_string(clock.nowMillis()));
        auto t = std::filesystem::last_write_time(dir.at("app/code/.deploy"));
        std::filesystem::last_write_time(dir.at("app/code/.deploy"), t + std::chrono::seconds(++deploys_));
    }

private:
    int deploys_ = 0;
};

std::string cookieFrom(const HttpResponse& r) {
    const std::string* set = r.headers.get("Set-Cookie");
    REQUIRE(set);
    return set->substr(0, set->find(';'));
}

void writeNotesApp(const TempDir& dir) {
    dir.write("app/code/migrations/001_create.sql",
              "CREATE TABLE notes (id INTEGER PRIMARY KEY, text TEXT NOT NULL);\n"
              "INSERT INTO notes (id, text) VALUES (1, 'first');\n");
    dir.write("app/code/queries/listNotes.sql", "SELECT * FROM notes ORDER BY id");
    dir.write("app/code/pages/notes.ms", "get.list = function(req)\n    return json(db.query(\"listNotes\"))\nend function\n");
}

}  // namespace

TEST_CASE("CgiApp: sessions carry across processes") {
    Site site;
    site.dir.write("app/code/pages/count.ms",
                   "get.up = function(req)\n"
                   "    if not session.hasIndex(\"n\") then session.n = 0\n"
                   "    session.n = session.n + 1\n"
                   "    return str(session.n)\n"
                   "end function\n");
    HttpResponse first = site.request("/count/up");
    REQUIRE(first.status == 200);
    CHECK(first.body == "1");
    std::string cookie = cookieFrom(first);
    CHECK(site.request("/count/up", cookie).body == "2");
    CHECK(site.request("/count/up", cookie).body == "3");
    CHECK(site.request("/count/up").body == "1");   // someone else
    CHECK(contains(logText(site.dir), "CGI mode: SSE push unavailable"));
}

TEST_CASE("CgiApp: a new version's migrations are applied before it's installed; a failure keeps the old one") {
    Site site;
    writeNotesApp(site.dir);
    HttpResponse list = site.request("/notes/list");
    REQUIRE(list.status == 200);
    CHECK(contains(list.body, "\"first\""));

    site.dir.write("app/code/migrations/002_stars.sql", "ALTER TABLE notes ADD COLUMN stars INTEGER DEFAULT 5;\n");
    site.deploy();
    list = site.request("/notes/list");
    CHECK(contains(list.body, "\"stars\":5"));
    CHECK(contains(logText(site.dir), "applied 1 migration (backup first:"));
    CHECK(std::filesystem::exists(site.dir.at("app/private/state/backups")));

    // A migration that fails: the database is as it was, and version 2 serves on.
    site.dir.write("app/code/migrations/003_bad.sql", "ALTER TABLE nowhere ADD COLUMN x INTEGER;\n");
    site.dir.write("app/code/pages/notes.ms", "get.list = function(req)\n    return \"version 3\"\nend function\n");
    site.deploy();
    list = site.request("/notes/list");
    CHECK(list.status == 200);
    CHECK(contains(list.body, "\"stars\":5"));
    std::string log = logText(site.dir);
    CHECK(contains(log, "migration failed"));
    CHECK(contains(log, "version 2 is still serving"));
}

TEST_CASE("CgiApp: requests wait while another process migrates, then get a 503") {
    Site site;
    site.dir.write("app/private/config/app.ms", "config = {}\nconfig.deploy = {\"migrationWaitSeconds\": 0.2}\n");
    site.dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>hi</body></html>");
    REQUIRE(site.request("/").status == 200);
    auto state = AppFolder::open(site.dir.at("app")).value().stateDir().value();
    SchemaFileLock lock(state);
    {
        auto held = lock.exclusive();
        REQUIRE(held.ok());
        HttpResponse r = site.request("/");
        CHECK(r.status == 503);
        CHECK(r.headers.get("Retry-After"));
    }
    CHECK(site.request("/").status == 200);
}

TEST_CASE("CgiApp: push channels aren't served") {
    Site site;
    site.dir.write("app/code/access.ms", "access = {}\naccess[\"/live/news\"] = \"public\"\n");
    site.dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>hi</body></html>");
    HttpResponse r = site.request("/live/news");
    CHECK(r.status != 200);
    CHECK_FALSE(r.stream);
}

TEST_CASE("cgiRequest: variables become the request") {
    CgiVariables vars{{"REQUEST_METHOD", "POST"},
                      {"REQUEST_URI", "/a/b?x=1"},
                      {"HTTP_HOST", "example.com"},
                      {"HTTP_X_CSRF_TOKEN", "t"},
                      {"HTTP_ACCEPT_LANGUAGE", "en"},
                      {"CONTENT_TYPE", "application/x-www-form-urlencoded"},
                      {"CONTENT_LENGTH", "3"},
                      {"REMOTE_ADDR", "198.51.100.7"},
                      {"SERVER_PROTOCOL", "HTTP/1.0"},
                      {"HTTPS", "on"}};
    HttpRequest r = cgiRequest(vars, "a=1");
    CHECK(r.method == "POST");
    CHECK(r.target == "/a/b?x=1");
    CHECK(*r.headers.get("X-Csrf-Token") == "t");
    CHECK(*r.headers.get("Accept-Language") == "en");
    CHECK(*r.headers.get("Content-Type") == "application/x-www-form-urlencoded");
    CHECK(*r.headers.get("Content-Length") == "3");
    CHECK(r.peerAddress == "198.51.100.7");
    CHECK(r.versionMinor == 0);
    CHECK(cgiHttps(vars));

    // Without REQUEST_URI: PATH_INFO and QUERY_STRING.
    HttpRequest p = cgiRequest({{"PATH_INFO", "/x"}, {"QUERY_STRING", "q=2"}}, "");
    CHECK(p.target == "/x?q=2");

    bool tooLarge = false;
    CHECK(cgiBodyLength(vars, {}, &tooLarge) == size_t(3));
    CHECK_FALSE(cgiBodyLength({{"CONTENT_LENGTH", "2000000"}}, {}, &tooLarge));
    CHECK(tooLarge);
    CHECK(cgiBodyLength({{"CONTENT_LENGTH", "2000000"}, {"CONTENT_TYPE", "multipart/form-data; boundary=x"}}, {},
                        &tooLarge) == size_t(2000000));
    CHECK_FALSE(cgiBodyLength({{"CONTENT_LENGTH", "12x"}}, {}, &tooLarge));
    CHECK_FALSE(tooLarge);

    HttpResponse resp = HttpResponse::text(404, "nope");
    std::string out = cgiResponse(resp);
    CHECK(out.rfind("Status: 404 Not Found\r\n", 0) == 0);
    CHECK(contains(out, "Content-Length: 4\r\n\r\nnope"));
}

TEST_CASE("CgiApp: publish succeeds, and goes nowhere") {
    Site site;
    site.dir.write("app/code/pages/news.ms", "get.post = function(req)\n    publish \"news\", \"hello\"\n"
                                             "    return \"published\"\nend function\n");
    HttpResponse r = site.request("/news/post");
    CHECK(r.status == 200);
    CHECK(r.body == "published");
}

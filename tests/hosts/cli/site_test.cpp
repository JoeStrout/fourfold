// End to end (M11.2): moving an existing site in. URL rules (code/routes.ms)
// through `fourfold dev`'s server, on a real socket.
#include <sstream>
#include <thread>

#include "app_server.h"
#include "browser.h"
#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::test::bodyOf;
using fourfold::test::rawFetch;
using fourfold::test::TempDir;
using fourfold::test::textBetween;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int statusOf(const std::string& response) { return response.size() > 12 ? std::atoi(response.c_str() + 9) : 0; }

std::string headerOf(const std::string& response, const std::string& name) {
    std::string head = response.substr(0, response.find("\r\n\r\n") + 2);
    if (!contains(head, "\r\n" + name + ": ")) return "(none)";
    return textBetween(head, "\r\n" + name + ": ", "\r\n");
}

struct Server {
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;

    explicit Server(const TempDir& dir) {
        server = AppServer::startDev(dir.at("app"), 0, out, err);
        REQUIRE_MESSAGE(server, err.str());
        REQUIRE_MESSAGE(server->compiled(), err.str());
        serving = std::thread([this] { server->loop().run(); });
    }
    ~Server() {
        server->loop().stop();
        serving.join();
    }

    std::string fetch(const std::string& target, const std::string& method = "GET",
                      const std::string& extraHeaders = "") {
        return rawFetch(server->port(), method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n" + extraHeaders +
                                            "Connection: close\r\n\r\n");
    }
};

void writeSite(const TempDir& dir) {
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>home</body></html>");
    dir.write("app/code/pages/tryit/index.html",
              "<!DOCTYPE html><html><body>code={{code}} path={{path}} n={{n}}</body></html>");
    dir.write("app/code/pages/tryit/index.ms",
              "render = function(req)\n"
              "  n = \"\"\n"
              "  if req.query.hasIndex(\"n\") then n = req.query.n\n"
              "  return {\"code\": req.query.s, \"path\": req.path, \"n\": n}\n"
              "end function\n");
    dir.write("app/code/pages/img/favicon/favicon.ico", "ICON");
    dir.write("app/code/pages/members/index.html", "<!DOCTYPE html><html><body>members</body></html>");
    dir.write("app/code/routes.ms",
              "redirects = {}\n"
              "redirects[\"/wiki/{rest*}\"] = \"https://wiki.miniscript.org/wiki/{rest}\"\n"
              "redirects[\"/old/{x}\"] = {\"to\": \"/tryit/?from={x}\", \"status\": 307}\n"
              "rewrites = {}\n"
              "rewrites[\"/tryit/{code}\"] = \"/tryit/?s={code}\"\n"
              "rewrites[\"/favicon.ico\"] = \"/img/favicon/favicon.ico\"\n"
              "rewrites[\"/club/{x}\"] = \"/members/\"\n");
    dir.write("app/code/access.ms", "rules = {}\nrules[\"/club\"] = \"login\"\n");
}

}  // namespace

TEST_CASE("URL rules: redirects keep the visitor's query; rewrites serve the target at the visitor's URL") {
    TempDir dir;
    writeSite(dir);
    Server s(dir);

    std::string r = s.fetch("/wiki/Main_Page?action=edit");
    CHECK_EQ(statusOf(r), 301);
    CHECK_EQ(headerOf(r, "Location"), "https://wiki.miniscript.org/wiki/Main_Page?action=edit");
    r = s.fetch("/wiki/a%20b/c%3Fd");
    CHECK_EQ(headerOf(r, "Location"), "https://wiki.miniscript.org/wiki/a%20b/c%3Fd");
    r = s.fetch("/old/x%26y?z=1");
    CHECK_EQ(statusOf(r), 307);
    CHECK_EQ(headerOf(r, "Location"), "/tryit/?from=x%26y&z=1");

    r = s.fetch("/tryit/AbCd1234?n=7");
    CHECK_EQ(statusOf(r), 200);
    CHECK_MESSAGE(contains(bodyOf(r), "code=AbCd1234 path=/tryit/AbCd1234 n=7"), r);
    // The target's parameters come first: the visitor can't replace them.
    r = s.fetch("/tryit/AbCd1234?s=other");
    CHECK_MESSAGE(contains(bodyOf(r), "code=AbCd1234 "), r);
    // The page itself still serves; a rule never takes over a route.
    r = s.fetch("/tryit/?s=direct");
    CHECK_MESSAGE(contains(bodyOf(r), "code=direct path=/tryit"), r);

    r = s.fetch("/favicon.ico");
    CHECK_EQ(statusOf(r), 200);
    CHECK_EQ(bodyOf(r), "ICON");
    CHECK_EQ(headerOf(r, "Content-Type"), "image/x-icon");

    // Rules answer GET and HEAD only.
    r = s.fetch("/tryit/AbCd1234", "POST", "Content-Length: 0\r\n");
    CHECK_EQ(statusOf(r), 405);
    CHECK_EQ(headerOf(r, "Allow"), "GET, HEAD");
    r = s.fetch("/tryit/AbCd1234", "HEAD");
    CHECK_EQ(statusOf(r), 200);
    CHECK(bodyOf(r).empty());

    // The visitor's URL's access rules apply as well as the target's.
    r = s.fetch("/members/");
    CHECK_EQ(statusOf(r), 200);
    r = s.fetch("/club/x");
    CHECK_EQ(statusOf(r), 303);
    CHECK_MESSAGE(contains(headerOf(r, "Location"), "/__auth/login?next=/club/x"), r);

    // Nothing matches: still not found.
    CHECK_EQ(statusOf(s.fetch("/tryit/a/b")), 404);

    // The dev tools' access report lists the rules, in the order tried.
    std::string report = bodyOf(s.fetch("/__dev/access.json"));
    size_t wiki = report.find("{\"url\":\"/wiki/{rest*}\",\"kind\":\"redirect 301 to https://wiki.miniscript.org/wiki/{rest}\"");
    size_t club = report.find("{\"url\":\"/club/{x}\",\"kind\":\"rewrite to /members/\",\"access\":\"login\"");
    CHECK_MESSAGE(wiki != std::string::npos, report);
    CHECK_MESSAGE(club != std::string::npos, report);
    CHECK(wiki < club);
}

namespace {

// A file's worth of bytes that differ from place to place, so a wrong
// offset shows.
std::string pattern(size_t size) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; i++) s[i] = static_cast<char>('a' + (i * 7 + i / 251) % 26);
    return s;
}

void writeFiles(const TempDir& dir, const std::string& big) {
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>home</body></html>");
    dir.write("app/code/pages/small.txt", "0123456789");
    dir.write("app/code/pages/Build/game.unityweb", big);
    dir.write("app/code/pages/news.xml", "<rss><channel><title>News</title></channel></rss>");
    dir.write("app/code/pages/members/secret.txt", "for members");
    dir.write("app/code/static.ms",
              "types = {}\n"
              "types[\"unityweb\"] = \"application/octet-stream\"\n"
              "types[\"xml\"] = \"application/rss+xml\"\n"
              "maxAge = {}\n"
              "maxAge[\"/Build\"] = 3600\n");
    dir.write("app/code/access.ms", "rules = {}\nrules[\"/members\"] = \"login\"\n");
}

}  // namespace

TEST_CASE("Static files: validators, 304s, ranges, and large files streamed") {
    TempDir dir;
    const std::string big = pattern(3 * 1024 * 1024 + 123);
    writeFiles(dir, big);
    Server s(dir);

    // Validators; revalidate by default.
    std::string r = s.fetch("/small.txt");
    CHECK_EQ(statusOf(r), 200);
    CHECK_EQ(bodyOf(r), "0123456789");
    std::string etag = headerOf(r, "ETag");
    std::string modified = headerOf(r, "Last-Modified");
    CHECK(etag.size() > 4);
    CHECK(etag.front() == '"');
    CHECK(contains(modified, " GMT"));
    CHECK_EQ(headerOf(r, "Accept-Ranges"), "bytes");
    CHECK_EQ(headerOf(r, "Cache-Control"), "no-cache");

    // Conditional requests.
    r = s.fetch("/small.txt", "GET", "If-None-Match: " + etag + "\r\n");
    CHECK_EQ(statusOf(r), 304);
    CHECK(bodyOf(r).empty());
    CHECK_EQ(headerOf(r, "ETag"), etag);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "If-None-Match: \"x\", W/" + etag + "\r\n")), 304);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "If-None-Match: \"other\"\r\n")), 200);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "If-Modified-Since: " + modified + "\r\n")), 304);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "If-Modified-Since: Sat, 01 Jan 2000 00:00:00 GMT\r\n")), 200);
    // If-None-Match wins over If-Modified-Since.
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET",
                              "If-None-Match: \"other\"\r\nIf-Modified-Since: " + modified + "\r\n")),
             200);

    // Ranges.
    r = s.fetch("/small.txt", "GET", "Range: bytes=2-5\r\n");
    CHECK_EQ(statusOf(r), 206);
    CHECK_EQ(bodyOf(r), "2345");
    CHECK_EQ(headerOf(r, "Content-Range"), "bytes 2-5/10");
    CHECK_EQ(headerOf(r, "Content-Length"), "4");
    CHECK_EQ(bodyOf(s.fetch("/small.txt", "GET", "Range: bytes=7-\r\n")), "789");
    CHECK_EQ(bodyOf(s.fetch("/small.txt", "GET", "Range: bytes=-3\r\n")), "789");
    CHECK_EQ(bodyOf(s.fetch("/small.txt", "GET", "Range: bytes=8-100\r\n")), "89");
    r = s.fetch("/small.txt", "GET", "Range: bytes=10-\r\n");
    CHECK_EQ(statusOf(r), 416);
    CHECK_EQ(headerOf(r, "Content-Range"), "bytes */10");
    // Several ranges, or a malformed one: the whole file.
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "Range: bytes=0-1,4-5\r\n")), 200);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "Range: bytes=5-2\r\n")), 200);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "Range: lines=1-2\r\n")), 200);
    // If-Range: the range only if the file is the one the client has part of.
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "Range: bytes=2-5\r\nIf-Range: " + etag + "\r\n")), 206);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "Range: bytes=2-5\r\nIf-Range: " + modified + "\r\n")), 206);
    CHECK_EQ(statusOf(s.fetch("/small.txt", "GET", "Range: bytes=2-5\r\nIf-Range: \"stale\"\r\n")), 200);

    // A large file, streamed: whole, in part, and by HEAD.
    r = s.fetch("/Build/game.unityweb");
    CHECK_EQ(statusOf(r), 200);
    CHECK_EQ(headerOf(r, "Content-Length"), std::to_string(big.size()));
    CHECK_EQ(headerOf(r, "Content-Type"), "application/octet-stream");
    CHECK_EQ(headerOf(r, "Cache-Control"), "public, max-age=3600");
    CHECK(bodyOf(r) == big);
    r = s.fetch("/Build/game.unityweb", "GET", "Range: bytes=1000000-\r\n");
    CHECK_EQ(statusOf(r), 206);
    CHECK(bodyOf(r) == big.substr(1000000));
    r = s.fetch("/Build/game.unityweb", "HEAD");
    CHECK_EQ(statusOf(r), 200);
    CHECK_EQ(headerOf(r, "Content-Length"), std::to_string(big.size()));
    CHECK(bodyOf(r).empty());

    // An added XML type is sandboxed.
    r = s.fetch("/news.xml");
    CHECK_EQ(headerOf(r, "Content-Type"), "application/rss+xml");
    CHECK(headerOf(r, "Content-Security-Policy").rfind("sandbox; default-src 'self'", 0) == 0);
    CHECK_EQ(headerOf(s.fetch("/small.txt"), "Content-Security-Policy").find("sandbox"), std::string::npos);

    // A restricted file is never cached.
    r = s.fetch("/members/secret.txt");
    CHECK_EQ(statusOf(r), 303);
}

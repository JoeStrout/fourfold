// End to end: the `file` module and RawData (M5.1) through `fourfold dev`'s
// server, with an app in a temporary folder.
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <thread>

#include "app_server.h"
#include "browser.h"
#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::test::bodyOf;
using fourfold::test::Browser;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Turns error values into "error: <message>", so json() can show them.
const char* kShow =
    "show = function(r)\n"
    "    for i in r.indexes\n"
    "        if r[i] isa error then r[i] = \"error: \" + r[i].message\n"
    "    end for\n"
    "    return json(r)\n"
    "end function\n";

void writeApp(const TempDir& dir) {
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><html><body>home</body></html>");
    dir.write("app/code/pages/f.ms",
              std::string(kShow) +
                  "get.text = function(req)\n"
                  "    r = []\n"
                  "    r.push file.readLines(\"notes/a.txt\")\n"
                  "    r.push file.writeLines(\"notes/a.txt\", [\"one\", \"\", \"caf\" + char(233)])\n"
                  "    r.push file.readLines(\"notes/a.txt\")\n"
                  "    r.push file.writeLines(\"notes/b.txt\", \"single\")\n"
                  "    r.push file.children(\"notes\")\n"
                  "    r.push file.children\n"
                  "    r.push file.exists(\"notes\")\n"
                  "    r.push file.exists(\"notes/c.txt\")\n"
                  "    i = file.info(\"notes/b.txt\")\n"
                  "    r.push [i.path, i.isDirectory, i.size, i.date.len]\n"
                  "    r.push file.info(\"notes\").isDirectory\n"
                  "    r.push file.info(\"nope\")\n"
                  "    r.push file.makedir(\"x/y\")\n"
                  "    r.push file.delete(\"x\")\n"
                  "    r.push file.delete(\"x/y\")\n"
                  "    r.push file.delete(\"x/y\")\n"
                  "    r.push file.delete(\"notes/b.txt\")\n"
                  "    r.push [file.name(\"a/b/c.txt\"), file.parent(\"a/b/c.txt\"), file.parent(\"c.txt\")]\n"
                  "    r.push file.child(\"a/b\", \"c.txt\")\n"
                  "    r.push file.child(\"\", \"c.txt\")\n"
                  "    return show(r)\n"
                  "end function\n"
                  "get.escape = function(req)\n"
                  "    r = []\n"
                  "    for p in [\"../secret.txt\", \"/etc/passwd\", \".fourfold/locks/x\", \"a/.hidden\", \"\", "
                  "\"a\\\\b\"]\n"
                  "        r.push file.readLines(p) isa error\n"
                  "        r.push file.writeLines(p, \"x\") isa error\n"
                  "    end for\n"
                  "    r.push file.readLines(\"link.txt\")\n"
                  "    r.push file.readLines(\"out/secret.txt\")\n"
                  "    r.push file.writeLines(\"out/new.txt\", \"x\") isa error\n"
                  "    r.push file.loadRaw(\"link.txt\")\n"
                  "    r.push file.children(\"out\") isa error\n"
                  "    r.push file.children.indexOf(\"out\")\n"
                  "    return show(r)\n"
                  "end function\n"
                  "get.binary = function(req)\n"
                  "    r = new RawData\n"
                  "    r.resize 28\n"
                  "    r.setByte 0, 255\n"
                  "    r.setSbyte 1, -2\n"
                  "    r.setUshort 2, 65535\n"
                  "    r.setShort 4, -3\n"
                  "    r.setUint 6, 4000000000\n"
                  "    r.setInt 10, -5\n"
                  "    r.setFloat 14, 1.5\n"
                  "    r.littleEndian = 0\n"
                  "    r.setDouble 20, -2.25\n"
                  "    file.saveRaw \"bin/r.dat\", r\n"
                  "    s = file.loadRaw(\"bin/r.dat\")\n"
                  "    out = [s.len, s.byte(0), s.sbyte(1), s.ushort(2), s.short(4), s.uint(6), s.int(10), "
                  "s.float(14)]\n"
                  "    s.littleEndian = 0\n"
                  "    out.push s.double(20)\n"
                  "    out.push s.byte(-28)\n"
                  "    out.push s.byte(28)\n"
                  "    out.push s.ushort(27)\n"
                  "    t = new RawData\n"
                  "    t.len = 8\n"
                  "    out.push t.setUtf8(0, \"h\" + char(233) + \"llo!!!\")\n"
                  "    out.push t.utf8\n"
                  "    out.push t.utf8(0, 2)\n"
                  "    out.push file.readLines(\"bin/r.dat\")\n"
                  "    return show(out)\n"
                  "end function\n"
                  "get.limits = function(req)\n"
                  "    out = []\n"
                  "    r = new RawData\n"
                  "    out.push r.resize(16 * 1024 * 1024 + 1)\n"
                  "    out.push r.len\n"
                  "    keep = []\n"
                  "    for i in range(1, 5)\n"
                  "        b = new RawData\n"
                  "        out.push b.resize(15000000)\n"
                  "        keep.push b\n"
                  "    end for\n"
                  "    keep = null\n"
                  "    for i in range(1, 10)\n"
                  "        b = new RawData\n"
                  "        b.resize 15000000\n"
                  "    end for\n"
                  "    out.push b.len\n"
                  "    return show(out)\n"
                  "end function\n"
                  "get.forged = function(req)\n"
                  "    out = []\n"
                  "    r = new RawData\n"
                  "    out.push r.len\n"
                  "    out.push r.byte(0)\n"
                  "    r._handle = markdown(\"x\")\n"
                  "    out.push r.len\n"
                  "    out.push r.byte(0)\n"
                  "    r._handle = 7\n"
                  "    out.push r.len\n"
                  "    out.push file.saveRaw(\"x.dat\", {\"_handle\": 1})\n"
                  "    out.push RawData.len\n"
                  "    return show(out)\n"
                  "end function\n"
                  "get.json = function(req)\n"
                  "    r = new RawData\n"
                  "    r.resize 4\n"
                  "    return json({\"r\": r})\n"
                  "end function\n"
                  "get.session = function(req)\n"
                  "    r = new RawData\n"
                  "    session.r = r\n"
                  "    return \"stored\"\n"
                  "end function\n"
                  "get.store = function(req)\n"
                  "    return str(data.set(\"r\", new RawData))\n"
                  "end function\n"
                  "slowly = function(x)\n"
                  "    wait 0.4\n"
                  "    return 1\n"
                  "end function\n"
                  "get.update = function(req)\n"
                  "    return str(data.update(\"n\", @slowly, 0))\n"
                  "end function\n"
                  "get.overwrite = function(req)\n"
                  "    return str(file.writeLines(\"n.json\", \"2\"))\n"
                  "end function\n"
                  "get.value = function(req)\n"
                  "    return str(data.get(\"n\"))\n"
                  "end function\n");
    dir.write("app/code/pages/about.html", "<!DOCTYPE html><html><body>{{text}}</body></html>");
    dir.write("app/code/pages/about.ms",
              "render = function(req)\n"
              "    return {\"text\": markdown(file.readLines(\"pages/about.md\"), \"full\")}\n"
              "end function\n");
    dir.write("app/data/pages/about.md", "# About us\n\nWe sell *things*.\n\n<script>alert(1)</script>\n");
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
    uint16_t port() const { return server->port(); }
};

}  // namespace

TEST_CASE("File: text files, folders and paths") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    std::string r = bodyOf(b.get("/f/text"));
    INFO(r);
    CHECK_EQ(r,
             "[null,3,[\"one\",\"\",\"caf\xc3\xa9\"],1,[\"a.txt\",\"b.txt\"],[\"notes\",\"pages\"],1,0,"
             "[\"notes/b.txt\",0,7,19],1,null,null,\"error: file.delete: data/x: not empty\",1,0,1,"
             "[\"c.txt\",\"a/b\",\"\"],\"a/b/c.txt\",\"c.txt\"]");
    CHECK_EQ(slurp(dir.at("app/data/notes/a.txt")), "one\n\ncaf\xc3\xa9\n");
}

TEST_CASE("File: paths can't leave data/") {
    TempDir dir;
    writeApp(dir);
    dir.write("secret.txt", "host secret");
    dir.makeDir("outside");
    dir.write("outside/secret.txt", "host secret");
    dir.symlink(dir.at("secret.txt"), "app/data/link.txt");
    dir.symlink(dir.at("outside"), "app/data/out");
    Server s(dir);
    Browser b(s.port());
    std::string r = bodyOf(b.get("/f/escape"));
    INFO(r);
    CHECK_EQ(r, "[1,1,1,1,1,1,1,1,1,1,1,1,null,null,1,null,1,null]");
    CHECK_FALSE(contains(r, "host secret"));
    struct stat st;
    CHECK(stat(dir.at("outside/new.txt").c_str(), &st) != 0);
    // Nothing in data/ is served by URL.
    for (const char* url : {"/data/pages/about.md", "/pages/about.md", "/about.md", "/data/notes"}) {
        INFO(url);
        CHECK(contains(b.get(url), "404 Not Found"));
    }
}

TEST_CASE("File: binary files through RawData") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    std::string r = bodyOf(b.get("/f/binary"));
    INFO(r);
    CHECK_EQ(r,
             "[28,255,-2,65535,-3,4000000000,-5,1.5,-2.25,255,\"error: RawData index out of bounds\","
             "\"error: RawData index out of bounds\",8,\"h\xc3\xa9llo!!\",\"h\xef\xbf\xbd\","
             "\"error: file.readLines: data/bin/r.dat isn't UTF-8 text (use file.loadRaw for binary files)\"]");
    std::string bytes = slurp(dir.at("app/data/bin/r.dat"));
    REQUIRE_EQ(bytes.size(), 28u);
    CHECK_EQ(static_cast<uint8_t>(bytes[0]), 0xFF);
    CHECK_EQ(static_cast<uint8_t>(bytes[6]), 0x00);   // 4000000000, little-endian
    CHECK_EQ(static_cast<uint8_t>(bytes[9]), 0xEE);
    CHECK_EQ(static_cast<uint8_t>(bytes[20]), 0xC0);   // -2.25, big-endian
}

TEST_CASE("File: RawData's limits, and forged handles") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    std::string limits = bodyOf(b.get("/f/limits"));
    INFO(limits);
    // One buffer over 16 MB; four of 15 MB fit in 64 MB, the fifth doesn't;
    // buffers no longer used are collected to make room.
    CHECK_EQ(limits,
             "[\"error: RawData can't be over 16777216 bytes\",0,null,null,null,null,"
             "\"error: a request's RawData can't total over 67108864 bytes\",15000000]");
    std::string forged = bodyOf(b.get("/f/forged"));
    INFO(forged);
    CHECK_EQ(forged,
             "[0,\"error: RawData has no buffer (use resize first)\",0,"
             "\"error: RawData has no buffer (use resize first)\",0,"
             "\"error: file.saveRaw needs a RawData (make one with new RawData)\",0]");
    CHECK(contains(b.get("/f/json"), "is RawData"));
    std::string session = b.get("/f/session");
    CHECK(contains(session, "500 Internal Server Error"));
    CHECK(contains(session, "is RawData"));
    CHECK(contains(bodyOf(b.get("/f/store")), "is RawData"));
}

TEST_CASE("File: a write waits for a data.update of the same file") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    uint16_t port = s.port();
    std::string updated;
    std::thread updater([port, &updated] {
        updated = test::rawFetch(port, "GET /f/update HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));   // the update holds the lock by now
    Browser b(port);
    CHECK_EQ(bodyOf(b.get("/f/overwrite")), "1");
    updater.join();
    CHECK_EQ(bodyOf(updated), "1");
    // The write landed after the update's, not under it (where the update
    // would have overwritten it).
    CHECK_EQ(bodyOf(b.get("/f/value")), "2");
}

TEST_CASE("File: a Markdown page kept in data/") {
    TempDir dir;
    writeApp(dir);
    Server s(dir);
    Browser b(s.port());
    std::string page = b.get("/about");
    INFO(bodyOf(page));
    CHECK(contains(page, ">About us</h1>"));
    CHECK(contains(page, "<em>things</em>"));
    CHECK_FALSE(contains(page, "<script>alert(1)</script>"));
    // A change to the file shows at once: it's data, not code.
    dir.write("app/data/pages/about.md", "# Changed\n");
    CHECK(contains(b.get("/about"), ">Changed</h1>"));
}

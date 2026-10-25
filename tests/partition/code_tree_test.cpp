#include "partition/code_tree.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

RelPath rel(const char* p) { return RelPath::parse(p).value(); }

CodeTree openCode(const TempDir& dir, CodeTreeLimits limits = {}) {
    auto app = AppFolder::open(dir.path(), AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto tree = app.value().codeTree(limits);
    REQUIRE(tree.ok());
    return std::move(tree).value();
}

std::vector<std::string> paths(const CodeListing& listing) {
    std::vector<std::string> result;
    for (const CodeEntry& e : listing.files) result.push_back(e.path.str());
    return result;
}

}  // namespace

TEST_CASE("CodeTree: lists regular files, sorted, skipping dot-files") {
    TempDir dir;
    dir.write("code/pages/index.html", "<html>home</html>");
    dir.write("code/pages/about.html", "<html>about</html>");
    dir.write("code/pages/members/photos/beach.jpg", "JPEG");
    dir.write("code/lib/util.ms", "x = 1");
    dir.write("code/.deploy", "");
    dir.write("code/pages/.DS_Store", "junk");
    dir.write("code/.git/config", "[core]");
    dir.makeDir("code/empty");
    CodeTree tree = openCode(dir);

    auto listing = tree.list();
    REQUIRE(listing.ok());
    CHECK(paths(listing.value()) == std::vector<std::string>{
                                        "lib/util.ms",
                                        "pages/about.html",
                                        "pages/index.html",
                                        "pages/members/photos/beach.jpg",
                                    });
    CHECK(listing.value().skipped.empty());
    CHECK_EQ(listing.value().files[1].size, 18u);
    CHECK(listing.value().files[1].modifiedNs > 0);
}

TEST_CASE("CodeTree: reads files") {
    TempDir dir;
    dir.write("code/pages/about.html", "<html>about</html>");
    dir.write("code/empty.txt", "");
    CodeTree tree = openCode(dir);

    auto file = tree.read(rel("pages/about.html"));
    REQUIRE(file.ok());
    CHECK_EQ(file.value().contents(), "<html>about</html>");
    CHECK_EQ(file.value().path().str(), "pages/about.html");

    auto empty = tree.read(rel("empty.txt"));
    REQUIRE(empty.ok());
    CHECK(empty.value().contents().empty());

    auto info = tree.stat(rel("pages/about.html"));
    REQUIRE(info.ok());
    CHECK_EQ(info.value().size, 18u);

    auto missing = tree.read(rel("pages/nope.html"));
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_EQ(missing.error().message, "code/pages/nope.html: not found");

    auto folder = tree.read(rel("pages"));
    REQUIRE_FALSE(folder.ok());
    CHECK(folder.error().code == ErrorCode::NotFound);
}

TEST_CASE("CodeTree: never follows symbolic links") {
    TempDir dir;
    dir.write("code/pages/real.html", "real");
    dir.write("data/upload.ms", "evil()");
    dir.write("secret.txt", "secret");
    dir.symlink("real.html", "code/pages/alias.html");                // link within code/
    dir.symlink("../../data/upload.ms", "code/pages/evil.ms");       // link into data/
    dir.symlink(dir.at("secret.txt"), "code/pages/secret.txt");      // absolute link out
    dir.symlink("../../data", "code/pages/datadir");                 // folder link
    CodeTree tree = openCode(dir);

    auto listing = tree.list();
    REQUIRE(listing.ok());
    CHECK(paths(listing.value()) == std::vector<std::string>{"pages/real.html"});
    CHECK(listing.value().skipped == std::vector<std::string>{
                                         "code/pages/alias.html: symbolic link (not followed)",
                                         "code/pages/datadir: symbolic link (not followed)",
                                         "code/pages/evil.ms: symbolic link (not followed)",
                                         "code/pages/secret.txt: symbolic link (not followed)",
                                     });

    // Reading through a link is "not found", like any other bad path.
    for (const char* p : {"pages/alias.html", "pages/evil.ms", "pages/secret.txt", "pages/datadir/upload.ms"}) {
        CAPTURE(p);
        auto r = tree.read(rel(p));
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::NotFound);
        CHECK(r.error().message.find(dir.path()) == std::string::npos);   // no host paths
    }
}

TEST_CASE("CodeTree: skips names RelPath rejects, and non-regular files") {
    TempDir dir;
    dir.write("code/pages/ok.html", "ok");
    dir.write("code/pages/back\\slash.html", "x");
    dir.write("code/pages/new\nline.html", "x");
    REQUIRE(mkfifo(dir.at("code/pages/pipe").c_str(), 0600) == 0);
    CodeTree tree = openCode(dir);

    auto listing = tree.list();
    REQUIRE(listing.ok());
    CHECK(paths(listing.value()) == std::vector<std::string>{"pages/ok.html"});
    CHECK(listing.value().skipped == std::vector<std::string>{
                                         "code/pages/back\\slash.html: backslash",
                                         "code/pages/new?line.html: control character",
                                         "code/pages/pipe: not a regular file",
                                     });

    auto pipe = tree.read(rel("pages/pipe"));   // must not hang
    REQUIRE_FALSE(pipe.ok());
    CHECK(pipe.error().code == ErrorCode::NotFound);
}

TEST_CASE("CodeTree: limits") {
    TempDir dir;
    dir.write("code/a.txt", "12345");
    dir.write("code/b.txt", "1");
    dir.write("code/c.txt", "1");

    CodeTreeLimits small;
    small.maxFileBytes = 4;
    small.maxFiles = 2;
    CodeTree tree = openCode(dir, small);

    auto big = tree.read(rel("a.txt"));
    REQUIRE_FALSE(big.ok());
    CHECK(big.error().code == ErrorCode::TooLarge);
    CHECK(tree.read(rel("b.txt")).ok());

    auto listing = tree.list();
    REQUIRE_FALSE(listing.ok());
    CHECK(listing.error().code == ErrorCode::TooLarge);

    TempDir deep;
    std::string nested = "code";
    for (int i = 0; i < 5; i++) nested += "/d";
    deep.write(nested + "/f.txt", "x");
    CodeTreeLimits shallow;
    shallow.maxDepth = 3;
    auto deepListing = openCode(deep, shallow).list();
    REQUIRE_FALSE(deepListing.ok());
    CHECK(deepListing.error().code == ErrorCode::TooLarge);
}

TEST_CASE("CodeTree: the deploy trigger") {
    TempDir dir;
    dir.write("code/pages/index.html", "home");
    CodeTree code = openCode(dir);
    CHECK_FALSE(code.deployTrigger());

    dir.write("code/.deploy", "");
    auto first = code.deployTrigger();
    REQUIRE(first);
    struct timespec times[2] = {{0, UTIME_OMIT}, {1'000'000'000, 0}};   // set mtime to a known value
    REQUIRE(utimensat(AT_FDCWD, dir.at("code/.deploy").c_str(), times, 0) == 0);
    auto second = code.deployTrigger();
    REQUIRE(second);
    CHECK_EQ(*second, int64_t(1'000'000'000) * 1'000'000'000);
    CHECK_NE(*first, *second);

    // Not a file in the listing.
    auto listing = code.list();
    REQUIRE(listing.ok());
    for (const CodeEntry& e : listing.value().files) CHECK_NE(e.path.str(), ".deploy");

    SUBCASE("a symbolic link counts as missing") {
        dir.write("elsewhere", "");
        REQUIRE(unlink(dir.at("code/.deploy").c_str()) == 0);
        dir.symlink("../elsewhere", "code/.deploy");
        CHECK_FALSE(code.deployTrigger());
    }
    SUBCASE("so does a folder") {
        REQUIRE(unlink(dir.at("code/.deploy").c_str()) == 0);
        dir.write("code/.deploy/x", "");
        CHECK_FALSE(code.deployTrigger());
    }
}

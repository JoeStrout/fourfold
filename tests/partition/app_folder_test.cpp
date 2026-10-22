#include "partition/app_folder.h"

#include <sys/stat.h>

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

bool isDir(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

mode_t modeOf(const std::string& path) {
    struct stat info;
    stat(path.c_str(), &info);
    return info.st_mode & 0777;
}

const char* kFolders[] = {"code", "data", "dropbox", "private", "private/config", "private/state"};

}  // namespace

TEST_CASE("AppFolder: Create makes the app folder and every partition") {
    TempDir dir;
    std::string root = dir.at("myapp");
    auto app = AppFolder::open(root, AppFolder::Missing::Create);
    REQUIRE(app.ok());
    CHECK_EQ(app.value().root(), root);
    for (const char* f : kFolders) {
        CAPTURE(f);
        CHECK(isDir(root + "/" + f));
    }
    CHECK_EQ(modeOf(root + "/private/state"), mode_t(0700));
    CHECK_EQ(modeOf(root + "/data"), mode_t(0700));

    // Opening again is fine, and needs nothing created.
    CHECK(AppFolder::open(root, AppFolder::Missing::Fail).ok());
}

TEST_CASE("AppFolder: Fail reports what's missing") {
    TempDir dir;
    auto none = AppFolder::open(dir.at("nope"));
    REQUIRE_FALSE(none.ok());
    CHECK(none.error().code == ErrorCode::NotFound);

    dir.makeDir("app/code");
    auto partial = AppFolder::open(dir.at("app"));
    REQUIRE_FALSE(partial.ok());
    CHECK(partial.error().code == ErrorCode::NotFound);
    CHECK(partial.error().message.find("missing data/") != std::string::npos);
}

TEST_CASE("AppFolder: a partition that's a file is an error") {
    TempDir dir;
    dir.write("app/data", "not a folder");
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
    REQUIRE_FALSE(app.ok());
    CHECK(app.error().code == ErrorCode::Invalid);
}

TEST_CASE("AppFolder: code/ may be a link to a folder elsewhere") {
    TempDir dir;
    dir.write("checkout/pages/index.html", "<html></html>");
    dir.makeDir("app");
    dir.symlink(dir.at("checkout"), "app/code");
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto tree = app.value().codeTree();
    REQUIRE(tree.ok());
    auto file = tree.value().read(RelPath::parse("pages/index.html").value());
    REQUIRE(file.ok());
    CHECK_EQ(file.value().contents(), "<html></html>");
}

TEST_CASE("AppFolder: partitions may not overlap") {
    SUBCASE("code/ pointing into data/") {
        TempDir dir;
        dir.makeDir("app/data/uploads");
        dir.symlink("data/uploads", "app/code");
        auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
        REQUIRE_FALSE(app.ok());
        CHECK(app.error().code == ErrorCode::Invalid);
        CHECK(app.error().message.find("code/") == 0);
    }
    SUBCASE("data/ pointing into code/") {
        TempDir dir;
        dir.makeDir("app/code/pages");
        dir.symlink("code/pages", "app/data");
        auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
        REQUIRE_FALSE(app.ok());
        CHECK(app.error().code == ErrorCode::Invalid);
    }
    SUBCASE("code/ pointing at the app folder itself") {
        TempDir dir;
        dir.makeDir("app");
        dir.symlink(".", "app/code");
        auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
        REQUIRE_FALSE(app.ok());
        CHECK(app.error().code == ErrorCode::Invalid);
    }
    SUBCASE("two partitions sharing one folder") {
        TempDir dir;
        dir.makeDir("shared");
        dir.makeDir("app");
        dir.symlink(dir.at("shared"), "app/data");
        dir.symlink(dir.at("shared"), "app/dropbox");
        auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
        REQUIRE_FALSE(app.ok());
        CHECK(app.error().code == ErrorCode::Invalid);
    }
}

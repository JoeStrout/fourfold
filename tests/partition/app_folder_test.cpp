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

TEST_CASE("AppFolder: a new private/config/ gets templates; an existing one is left alone") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("new"), AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto config = app.value().configDir();
    REQUIRE(config.ok());
    auto appMs = config.value().read("app.ms");
    REQUIRE(appMs.ok());
    REQUIRE(appMs.value().has_value());
    CHECK(appMs.value()->contents().find("config = {}") != std::string::npos);
    CHECK_EQ(appMs.value()->name(), "private/config/app.ms");
    auto secrets = config.value().read("secrets.ms");
    REQUIRE(secrets.ok());
    REQUIRE(secrets.value().has_value());
    CHECK(secrets.value()->contents().find("hosts") != std::string::npos);

    dir.makeDir("old/private/config");
    auto old = AppFolder::open(dir.at("old"), AppFolder::Missing::Create);
    REQUIRE(old.ok());
    auto none = old.value().configDir().value().read("app.ms");
    REQUIRE(none.ok());
    CHECK_FALSE(none.value().has_value());
}

TEST_CASE("ConfigDir: reads plain files only, and stamps them") {
    TempDir dir;
    dir.write("app/private/config/app.ms", "config = {}\n");
    dir.write("elsewhere.ms", "config = {\"x\": 1}\n");
    dir.symlink(dir.at("elsewhere.ms"), "app/private/config/secrets.ms");
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create);
    REQUIRE(app.ok());
    auto config = app.value().configDir().value();

    CHECK_EQ(config.read("app.ms").value()->contents(), "config = {}\n");
    auto link = config.read("secrets.ms");
    REQUIRE_FALSE(link.ok());
    CHECK(link.error().code == ErrorCode::NotFound);
    CHECK_FALSE(config.read("../x.ms").ok());
    CHECK_FALSE(config.read("sub/app.ms").ok());

    std::string before = config.stamp("app.ms");
    CHECK_FALSE(before.empty());
    CHECK(config.stamp("missing.ms").empty());
    dir.write("app/private/config/app.ms", "config = {\"domain\": \"example.com\"}\n");
    CHECK(config.stamp("app.ms") != before);

    auto server = ConfigDir::readServerConfig(dir.at("elsewhere.ms"));
    REQUIRE(server.ok());
    CHECK_EQ(server.value().name(), dir.at("elsewhere.ms"));
    CHECK_FALSE(ConfigDir::readServerConfig(dir.at("nope.ms")).ok());
}

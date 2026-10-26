#include "partition/data_dir.h"

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

RelPath rel(const char* s) { return RelPath::parse(s).value(); }

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

mode_t modeOf(const std::string& path) {
    struct stat info;
    stat(path.c_str(), &info);
    return info.st_mode & 0777;
}

struct App {
    TempDir dir;
    AppFolder folder = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    std::string at(const std::string& r) const { return dir.at("app/" + r); }
};

}  // namespace

TEST_CASE("DataDir: write, read, overwrite, remove") {
    App app;
    DataDir data = app.folder.dataDir().value();
    CHECK(data.read(rel("a.json"), 100).error().code == ErrorCode::NotFound);
    CHECK_FALSE(data.exists(rel("a.json")).value());
    REQUIRE(data.write(rel("a.json"), "one"));
    CHECK_EQ(data.read(rel("a.json"), 100).value(), "one");
    REQUIRE(data.write(rel("a.json"), "two!"));
    CHECK_EQ(slurp(app.at("data/a.json")), "two!");
    CHECK(data.exists(rel("a.json")).value());
    CHECK(data.read(rel("a.json"), 3).error().code == ErrorCode::TooLarge);
    REQUIRE(data.remove(rel("a.json")));
    CHECK(data.remove(rel("a.json")).error().code == ErrorCode::NotFound);
}

TEST_CASE("DataDir: folders are created; no temporary files are left") {
    App app;
    DataDir data = app.folder.dataDir().value();
    REQUIRE(data.write(rel("polls/2026/lunch.json"), "{}"));
    CHECK_EQ(slurp(app.at("data/polls/2026/lunch.json")), "{}");
    auto top = data.list(RelPath()).value();
    REQUIRE_EQ(top.size(), 1u);
    CHECK_EQ(top[0].name, "polls");
    CHECK(top[0].isFolder);
    auto inner = data.list(rel("polls/2026")).value();
    REQUIRE_EQ(inner.size(), 1u);   // no .tmp- files
    CHECK_EQ(inner[0].name, "lunch.json");
    CHECK(data.list(rel("nowhere")).value().empty());
}

TEST_CASE("DataDir: append adds to the end, and creates the file") {
    App app;
    DataDir data = app.folder.dataDir().value();
    REQUIRE(data.append(rel("log/book.jsonl"), "1\n"));
    REQUIRE(data.append(rel("log/book.jsonl"), "2\n"));
    CHECK_EQ(data.read(rel("log/book.jsonl"), 100).value(), "1\n2\n");
}

TEST_CASE("DataDir: concurrent appends don't interleave") {
    App app;
    DataDir data = app.folder.dataDir().value();
    std::vector<std::thread> threads;
    std::vector<std::string> errors(8);   // doctest's checks aren't thread-safe
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&data, &errors, t] {
            std::string line(200, static_cast<char>('a' + t));
            line += '\n';
            for (int i = 0; i < 100; i++) {
                Status s = data.append(RelPath::parse("x.jsonl").value(), line);
                if (!s) errors[t] = s.error().toString();
            }
        });
    }
    for (auto& th : threads) th.join();
    for (const auto& e : errors) CHECK_EQ(e, "");
    std::istringstream in(data.read(rel("x.jsonl"), 1 << 20).value());
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        CHECK_EQ(line.size(), 200u);
        CHECK_EQ(line.find_first_not_of(line[0]), std::string::npos);
        count++;
    }
    CHECK_EQ(count, 800);
}

TEST_CASE("DataDir: links and the engine's folder are out of reach") {
    App app;
    DataDir data = app.folder.dataDir().value();
    app.dir.write("secret.txt", "host secret");
    app.dir.symlink(app.dir.at("secret.txt"), "app/data/link.json");
    CHECK(data.read(rel("link.json"), 100).error().code == ErrorCode::NotFound);
    // Writing over a link replaces the link, never the file it points to.
    app.dir.symlink(app.dir.at("secret.txt"), "app/data/link2.json");
    CHECK(data.append(rel("link2.json"), "x").error().code == ErrorCode::NotFound);
    CHECK_EQ(slurp(app.dir.at("secret.txt")), "host secret");
    app.dir.symlink(app.dir.path(), "app/data/out");
    CHECK_FALSE(data.write(rel("out/evil.json"), "x"));
    CHECK(data.list(RelPath()).value().empty());   // the links aren't listed
    // A folder can't be overwritten by a file.
    app.dir.makeDir("app/data/folder");
    CHECK(data.write(rel("folder"), "x").error().code == ErrorCode::Conflict);
    // Lock files live in data/.fourfold/, which RelPath can't name.
    REQUIRE(data.lockFile("counter"));
    auto listed = data.list(RelPath()).value();
    for (const auto& e : listed) CHECK_NE(e.name, ".fourfold");
}

TEST_CASE("FileLock: exclusive between holders") {
    App app;
    DataDir data = app.folder.dataDir().value();
    auto first = FileLock::tryAcquire(data.lockFile("k").value());
    REQUIRE(first);
    REQUIRE(first.value().has_value());
    auto second = FileLock::tryAcquire(data.lockFile("k").value());
    REQUIRE(second);
    CHECK_FALSE(second.value().has_value());
    auto other = FileLock::tryAcquire(data.lockFile("other").value());
    CHECK(other.value().has_value());
    first.value().reset();   // released
    auto third = FileLock::tryAcquire(data.lockFile("k").value());
    CHECK(third.value().has_value());
}

TEST_CASE("Dropbox: append only") {
    App app;
    Dropbox box = app.folder.dropbox().value();
    REQUIRE(box.append(rel("app.log"), "one\n"));
    REQUIRE(box.append(rel("app.log"), "two\n"));
    CHECK_EQ(slurp(app.at("dropbox/app.log")), "one\ntwo\n");
}

TEST_CASE("StateDir: private files and SQLite paths") {
    App app;
    StateDir state = app.folder.stateDir().value();
    REQUIRE(state.write(rel("secret.key"), "k"));
    CHECK_EQ(modeOf(app.at("private/state/secret.key")), 0600);
    CHECK_EQ(state.read(rel("secret.key"), 10).value(), "k");
    REQUIRE(state.append(rel("logs/engine.log"), "x\n"));
    CHECK_EQ(modeOf(app.at("private/state/logs/engine.log")), 0600);
    auto db = state.sqlitePath(rel("db/state.db"));
    REQUIRE(db);
    char real[PATH_MAX];
    REQUIRE(realpath(app.at("private/state").c_str(), real));
    CHECK_EQ(db.value(), std::string(real) + "/db/state.db");
    struct stat info;
    CHECK(stat(app.at("private/state/db").c_str(), &info) == 0);
    REQUIRE(state.lockFile("session"));
}

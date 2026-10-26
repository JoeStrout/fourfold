#include "appdata/data_files.h"

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

struct Fixture {
    TempDir dir;
    AppFolder app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    DataFiles data{app.dataDir().value()};
};

RelPath name(const char* s) { return DataFiles::checkName(s).value(); }

// Holds a name's lock for the length of a scope.
class Held {
public:
    Held(DataFiles& data, const RelPath& n) : data_(data) {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        ticket_ = data.locks().acquire(DataFiles::valueLockKey(n), [&](Status, LockService::Ticket) {
            std::lock_guard<std::mutex> lock(m);
            done = true;
            cv.notify_all();
        });
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return done; });
    }
    ~Held() { data_.locks().release(ticket_); }

private:
    DataFiles& data_;
    LockService::Ticket ticket_;
};

}  // namespace

TEST_CASE("DataFiles: names") {
    CHECK(DataFiles::checkName("guestbook"));
    CHECK(DataFiles::checkName("polls/lunch"));
    CHECK(DataFiles::checkName("a.b"));
    for (const char* bad : {"", "/x", "x/", "../x", "a/../b", ".hidden", "a//b", "x.json", "x.jsonl", "a\\b"}) {
        INFO(bad);
        CHECK_FALSE(DataFiles::checkName(bad));
    }
    CHECK(DataFiles::checkFolder("").value().isRoot());
    CHECK_EQ(DataFiles::checkFolder("polls/").value().str(), "polls");
    CHECK_FALSE(DataFiles::checkFolder("../x"));
}

TEST_CASE("DataFiles: values") {
    Fixture f;
    CHECK_FALSE(f.data.get(name("x")).value());
    CHECK_FALSE(f.data.exists(name("x")).value());
    Datum v = Datum::map({{"a", Datum::list({Datum::number(1), Datum::string("two")})}});
    REQUIRE(f.data.set(name("x"), v));
    CHECK(*f.data.get(name("x")).value() == v);
    CHECK(f.data.exists(name("x")).value());
    REQUIRE(f.data.set(name("polls/lunch"), Datum::number(3)));
    CHECK_EQ(f.data.names(RelPath()).value(), std::vector<std::string>{"polls/", "x"});
    CHECK_EQ(f.data.names(DataFiles::checkFolder("polls").value()).value(), std::vector<std::string>{"lunch"});
    CHECK(f.data.remove(name("x")).value());
    CHECK_FALSE(f.data.remove(name("x")).value());
    CHECK_FALSE(f.data.get(name("x")).value());
}

TEST_CASE("DataFiles: logs") {
    Fixture f;
    REQUIRE(f.data.append(name("book"), Datum::map({{"who", Datum::string("Ada\nLovelace")}})));
    REQUIRE(f.data.append(name("book"), Datum::number(2)));
    auto got = f.data.get(name("book")).value();
    REQUIRE(got);
    REQUIRE_EQ(got->asList().size(), 2u);
    CHECK_EQ(got->asList()[0].asMap().at("who").asString(), "Ada\nLovelace");
    // A cut-off last line (a crash mid-append) is left out.
    {
        std::ofstream out(f.dir.at("app/data/book.jsonl"), std::ios::app);
        out << "{\"who\":\"Bo";
    }
    CHECK_EQ(f.data.get(name("book")).value()->asList().size(), 2u);
    // The kinds don't mix.
    CHECK(f.data.set(name("book"), Datum()).error().code == ErrorCode::Conflict);
    REQUIRE(f.data.set(name("val"), Datum()));
    CHECK(f.data.append(name("val"), Datum()).error().code == ErrorCode::Conflict);
    CHECK_EQ(f.data.names(RelPath()).value(), std::vector<std::string>{"book", "val"});
}

TEST_CASE("DataFiles: a damaged file is an error, not a guess") {
    Fixture f;
    f.dir.write("app/data/bad.json", "{not json");
    CHECK(f.data.get(name("bad")).error().code == ErrorCode::Invalid);
    f.dir.write("app/data/badlog.jsonl", "1\nnope\n3\n");
    CHECK(f.data.get(name("badlog")).error().code == ErrorCode::Invalid);
}

TEST_CASE("DataFiles: size limit") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    DataLimits limits;
    limits.maxBytes = 10;
    DataFiles data(app.dataDir().value(), limits);
    CHECK(data.set(name("x"), Datum::string("short")));
    CHECK(data.set(name("x"), Datum::string("much too long")).error().code == ErrorCode::TooLarge);
}

TEST_CASE("DataFiles: increment and compareAndSet") {
    Fixture f;
    {
        Held held(f.data, name("n"));
        CHECK_EQ(f.data.increment(name("n"), 1).value(), 1);
        CHECK_EQ(f.data.increment(name("n"), 2.5).value(), 3.5);
    }
    REQUIRE(f.data.set(name("s"), Datum::string("x")));
    CHECK(f.data.increment(name("s"), 1).error().code == ErrorCode::Conflict);

    CHECK(f.data.compareAndSet(name("c"), Datum(), Datum::number(1)).value());          // missing matches null
    CHECK_FALSE(f.data.compareAndSet(name("c"), Datum(), Datum::number(2)).value());
    CHECK(f.data.compareAndSet(name("c"), Datum::number(1), Datum::number(2)).value());
    CHECK_EQ(f.data.get(name("c")).value()->asNumber(), 2);
}

TEST_CASE("DataFiles: concurrent increments under the lock lose nothing") {
    Fixture f;
    RelPath n = name("visits");
    std::vector<std::thread> threads;
    std::vector<std::string> errors(8);
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&f, &n, &errors, t] {
            for (int i = 0; i < 50; i++) {
                Held held(f.data, n);
                auto r = f.data.increment(n, 1);
                if (!r) errors[t] = r.error().toString();
            }
        });
    }
    for (auto& th : threads) th.join();
    for (const auto& e : errors) CHECK_EQ(e, "");
    CHECK_EQ(f.data.get(n).value()->asNumber(), 400);
}

TEST_CASE("DataFiles: file paths") {
    CHECK_EQ(DataFiles::checkPath("notes/about.md", false).value().str(), "notes/about.md");
    CHECK(DataFiles::checkPath("votes.json", false));
    CHECK(DataFiles::checkPath("", true).value().isRoot());
    for (const char* bad : {"", "/etc/passwd", "/x", "../x", "a/../b", ".hidden", "a/.hidden", ".fourfold/locks/x",
                            "a//b", "a\\b", "x/"}) {
        INFO(bad);
        CHECK_FALSE(DataFiles::checkPath(bad, false));
    }
    CHECK_FALSE(DataFiles::checkPath("..", true));
}

TEST_CASE("DataFiles: lock keys are shared between file and data") {
    RelPath votes = name("votes");
    CHECK_EQ(DataFiles::valueLockKey(votes), DataFiles::lockKey(DataFiles::checkPath("votes.json", false).value()));
    CHECK_EQ(DataFiles::lockKey(DataFiles::checkPath("Polls/Votes.JSON", false).value()), "polls/votes.json");
}

TEST_CASE("DataFiles: whole files") {
    Fixture f;
    RelPath about = DataFiles::checkPath("pages/about.md", false).value();
    CHECK_FALSE(f.data.readFile(about).value());
    REQUIRE(f.data.writeFile(about, "# About\n"));
    CHECK_EQ(*f.data.readFile(about).value(), "# About\n");
    std::string binary("\x00\x01\xff", 3);
    RelPath raw = DataFiles::checkPath("raw.bin", false).value();
    REQUIRE(f.data.writeFile(raw, binary));
    CHECK_EQ(*f.data.readFile(raw).value(), binary);

    auto info = f.data.info(about).value();
    REQUIRE(info);
    CHECK_EQ(info->size, 8u);
    CHECK(f.data.info(about.parent()).value()->isFolder);
    CHECK_EQ(f.data.children(RelPath()).value(), std::vector<std::string>{"pages", "raw.bin"});
    CHECK(f.data.children(about).error().code == ErrorCode::Conflict);
    CHECK(f.data.children(DataFiles::checkPath("nope", false).value()).error().code == ErrorCode::NotFound);
    CHECK(f.data.readFile(about.parent()).error().code == ErrorCode::Conflict);

    REQUIRE(f.data.makeFolder(DataFiles::checkPath("empty", false).value()));
    CHECK_FALSE(f.data.removeFile(about.parent()));   // not empty
    CHECK(f.data.removeFile(about).value());
    CHECK_FALSE(f.data.removeFile(about).value());
    CHECK(f.data.removeFile(about.parent()).value());
    CHECK(f.data.removeFile(DataFiles::checkPath("empty", false).value()).value());
}

TEST_CASE("DataFiles: file and data see the same files") {
    Fixture f;
    RelPath votes = DataFiles::checkPath("votes.json", false).value();
    REQUIRE(f.data.set(name("votes"), Datum::map({{"yes", Datum::number(2)}})));
    CHECK_EQ(*f.data.readFile(votes).value(), "{\"yes\":2}");
    REQUIRE(f.data.writeFile(votes, "[1, 2]"));
    CHECK_EQ(toJson(*f.data.get(name("votes")).value()), "[1,2]");
    REQUIRE(f.data.writeFile(votes, "not json"));
    CHECK_FALSE(f.data.get(name("votes")));
}

TEST_CASE("DataFiles: the size cap covers files") {
    TempDir dir;
    AppFolder app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    DataLimits limits;
    limits.maxBytes = 10;
    DataFiles data(app.dataDir().value(), limits);
    RelPath p = DataFiles::checkPath("a.txt", false).value();
    CHECK(data.writeFile(p, "0123456789"));
    CHECK(data.writeFile(p, "0123456789x").error().code == ErrorCode::TooLarge);
    std::ofstream(dir.at("app/data/big.txt")) << "0123456789x";
    CHECK(data.readFile(DataFiles::checkPath("big.txt", false).value()).error().code == ErrorCode::TooLarge);
}

TEST_CASE("DataFiles: lines") {
    using V = std::vector<std::string>;
    CHECK_EQ(DataFiles::splitLines("").value(), V{});
    CHECK_EQ(DataFiles::splitLines("a").value(), V{"a"});
    CHECK_EQ(DataFiles::splitLines("a\n").value(), V{"a"});
    CHECK_EQ(DataFiles::splitLines("a\n\nb").value(), V{"a", "", "b"});
    CHECK_EQ(DataFiles::splitLines("a\r\nb\rc\n").value(), V{"a", "b", "c"});
    CHECK_EQ(DataFiles::splitLines("\n").value(), V{""});
    CHECK_EQ(DataFiles::splitLines("caf\xc3\xa9").value(), V{"caf\xc3\xa9"});
    CHECK(DataFiles::splitLines(std::string("a\0b", 3)).error().code == ErrorCode::Invalid);
    CHECK(DataFiles::splitLines("\xff\xfe").error().code == ErrorCode::Invalid);
    CHECK_EQ(DataFiles::joinLines({"a", "", "b"}), "a\n\nb\n");
    CHECK_EQ(DataFiles::joinLines({}), "");
}

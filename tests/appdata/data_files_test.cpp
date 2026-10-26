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
        ticket_ = data.locks().acquire(DataFiles::lockKey(n), [&](Status, LockService::Ticket) {
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

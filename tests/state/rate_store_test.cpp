#include "state/rate_store.h"

#include <atomic>
#include <thread>
#include <vector>

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

struct Fixture {
    TempDir dir;
    // At the start of a minute, so a test's windows line up.
    ManualClock clock{1'700'000'040'000};
    std::unique_ptr<RateStore> store;

    Fixture() { store = RateStore::open(dir.at("state.db"), clock).value(); }

    bool hit(const std::string& key, int64_t count, int64_t seconds = 60) {
        auto r = store->hit({{key, count, seconds}});
        REQUIRE(r);
        return r.value().allowed;
    }
};

}  // namespace

TEST_CASE("RateStore: counts up to the limit, then refuses, per key") {
    Fixture f;
    for (int i = 0; i < 5; i++) CHECK(f.hit("a", 5));
    CHECK_FALSE(f.hit("a", 5));
    CHECK_FALSE(f.hit("a", 5));
    CHECK(f.hit("b", 5));   // another key has its own count

    auto r = f.store->hit({{"a", 5, 60}});
    REQUIRE(r);
    CHECK_FALSE(r.value().allowed);
    CHECK_GE(r.value().retryAfterSeconds, 1);
    CHECK_LE(r.value().retryAfterSeconds, 60);
}

TEST_CASE("RateStore: a sliding window, estimated from two fixed ones") {
    Fixture f;
    for (int i = 0; i < 10; i++) CHECK(f.hit("k", 10));
    CHECK_FALSE(f.hit("k", 10));
    // Into the next window: the previous one's count still weighs, less as
    // time passes, so the limit doesn't reset all at once at the boundary.
    f.clock.advanceSeconds(60);
    CHECK_FALSE(f.hit("k", 10));       // 10 * 1.0 + 0
    f.clock.advanceSeconds(30);        // halfway: 10 * 0.5 = 5
    for (int i = 0; i < 4; i++) CHECK(f.hit("k", 10));   // 5 + 4 = 9 (+1 = 10 allowed at most)
    CHECK(f.hit("k", 10));
    CHECK_FALSE(f.hit("k", 10));
    f.clock.advanceSeconds(120);       // two windows on: forgotten
    for (int i = 0; i < 10; i++) CHECK(f.hit("k", 10));
}

TEST_CASE("RateStore: several checks count together or not at all") {
    Fixture f;
    // A tight limit and a loose one: once the tight one refuses, the loose
    // one isn't counted either.
    for (int i = 0; i < 3; i++) {
        auto r = f.store->hit({{"loose", 100, 60}, {"tight", 3, 60}});
        REQUIRE(r);
        CHECK(r.value().allowed);
    }
    auto refused = f.store->hit({{"loose", 100, 60}, {"tight", 3, 60}});
    REQUIRE(refused);
    CHECK_FALSE(refused.value().allowed);
    CHECK_EQ(refused.value().refusedIndex, 1u);
    auto counters = f.store->counters().value();
    double loose = 0;
    for (const auto& c : counters) {
        if (c.key == "loose") loose = c.count;
    }
    CHECK_EQ(loose, doctest::Approx(3));
}

TEST_CASE("RateStore: exact under concurrency, from several connections") {
    Fixture f;
    // A second store on the same file stands for another process.
    auto other = RateStore::open(f.dir.at("state.db"), f.clock).value();
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t] {
            RateStore& s = t % 2 ? *other : *f.store;
            for (int i = 0; i < 25; i++) {
                auto r = s.hit({{"shared", 50, 3600}});
                if (r && r.value().allowed) allowed++;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(allowed.load(), 50);
}

TEST_CASE("RateStore: a puzzle is good once, until it expires") {
    Fixture f;
    int64_t expires = f.clock.nowMillis() + 1000;
    CHECK(f.store->useOnce("puzzle-1", expires).value());
    CHECK_FALSE(f.store->useOnce("puzzle-1", expires).value());
    CHECK(f.store->useOnce("puzzle-2", expires).value());
    f.clock.advanceSeconds(2);
    CHECK_GE(f.store->sweep().value(), 2);
}

TEST_CASE("RateStore: sweep forgets old counters; reset forgets all") {
    Fixture f;
    CHECK(f.hit("old", 1));
    CHECK_FALSE(f.hit("old", 1));
    f.clock.advanceSeconds(180);
    CHECK_GE(f.store->sweep().value(), 1);
    CHECK(f.store->counters().value().empty());
    CHECK(f.hit("x", 1));
    REQUIRE(f.store->resetCounters());
    CHECK(f.hit("x", 1));
    // A long key is kept by its hash, and still counted.
    std::string longKey(500, 'z');
    CHECK(f.hit(longKey, 1));
    CHECK_FALSE(f.hit(longKey, 1));
}

TEST_CASE("RateStore: a request can count as more than one (app mail's recipients)") {
    Fixture f;
    auto hit = [&](int64_t cost) {
        auto r = f.store->hit({{"mail", 5, 3600, cost}});
        REQUIRE(r);
        return r.value().allowed;
    };
    CHECK(hit(3));
    CHECK_FALSE(hit(3));   // 6 would be over 5: nothing counted
    CHECK(hit(2));         // exactly 5
    CHECK_FALSE(hit(1));
}

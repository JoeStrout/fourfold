#include "state/lock_service.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "doctest.h"
#include "partition/app_folder.h"
#include "partition/file_lock.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

// Records grants, and lets a test wait for them.
struct Grants {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int> order;
    std::vector<bool> ok;

    LockService::Granted make(int n) {
        return [this, n](Status s, LockService::Ticket) {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(n);
            ok.push_back(static_cast<bool>(s));
            cv.notify_all();
        };
    }
    bool waitFor(size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return order.size() >= count; });
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return order.size();
    }
};

void checkFifo(LockService& locks) {
    Grants g;
    auto a = locks.acquire("k", g.make(1));
    REQUIRE(g.waitFor(1));
    auto b = locks.acquire("k", g.make(2));
    auto c = locks.acquire("k", g.make(3));
    auto other = locks.acquire("other", g.make(9));   // a different key doesn't wait
    REQUIRE(g.waitFor(2));
    CHECK_EQ(g.order[1], 9);
    CHECK_EQ(locks.stats().held, 2u);
    CHECK_EQ(locks.stats().waiting, 2u);
    locks.release(a);
    REQUIRE(g.waitFor(3));
    CHECK_EQ(g.order[2], 2);
    locks.release(b);
    REQUIRE(g.waitFor(4));
    CHECK_EQ(g.order[3], 3);
    locks.release(c);
    locks.release(other);
    CHECK_EQ(locks.stats().held, 0u);
    CHECK_EQ(locks.stats().waiting, 0u);
}

void checkGiveUp(LockService& locks) {
    Grants g;
    auto a = locks.acquire("k", g.make(1));
    REQUIRE(g.waitFor(1));
    auto b = locks.acquire("k", g.make(2));
    auto c = locks.acquire("k", g.make(3));
    locks.release(b);   // gives up while waiting: never granted
    locks.release(a);
    REQUIRE(g.waitFor(2));
    CHECK_EQ(g.order[1], 3);
    locks.release(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(g.size(), 2u);
    CHECK_EQ(locks.stats().waiting, 0u);
}

}  // namespace

TEST_CASE("LockService in memory: FIFO per key") {
    auto locks = makeMemoryLockService();
    checkFifo(*locks);
    checkGiveUp(*locks);
}

TEST_CASE("LockService with lock files: FIFO per key, and exclusive with other holders of the file") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    DataDir data = app.dataDir().value();
    WorkerPool workers(2);
    auto locks = makeFileLockService([&data](const std::string& key) { return data.lockFile(key); }, workers);
    checkFifo(*locks);
    checkGiveUp(*locks);

    // Another holder of the file (as another process would be) makes it wait.
    auto outside = FileLock::tryAcquire(data.lockFile("shared").value());
    REQUIRE(outside.value().has_value());
    Grants g;
    auto t = locks->acquire("shared", g.make(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(g.size(), 0u);
    outside.value().reset();
    REQUIRE(g.waitFor(1));
    CHECK(g.ok[0]);
    locks->release(t);

    // Released while still waiting on the file: never granted, and the next
    // waiter gets it.
    auto outside2 = FileLock::tryAcquire(data.lockFile("shared").value());
    REQUIRE(outside2.value().has_value());
    Grants g2;
    auto first = locks->acquire("shared", g2.make(1));
    auto second = locks->acquire("shared", g2.make(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    locks->release(first);
    outside2.value().reset();
    REQUIRE(g2.waitFor(1));
    CHECK_EQ(g2.order[0], 2);
    locks->release(second);
}

TEST_CASE("LockService: many threads, one key, no overlap") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    DataDir data = app.dataDir().value();
    WorkerPool workers(4);
    for (int kind = 0; kind < 2; kind++) {
        auto locks = kind == 0 ? makeMemoryLockService()
                               : makeFileLockService([&data](const std::string& k) { return data.lockFile(k); },
                                                     workers);
        std::atomic<int> inside{0};
        std::atomic<int> maxInside{0};
        std::atomic<int> done{0};
        constexpr int kTotal = 200;
        for (int i = 0; i < kTotal; i++) {
            auto ticket = std::make_shared<LockService::Ticket>();
            auto mutex = std::make_shared<std::mutex>();
            std::lock_guard<std::mutex> hold(*mutex);
            LockService* l = locks.get();
            *ticket = l->acquire("k", [&, l, ticket, mutex](Status, LockService::Ticket) {
                // Released from another thread, as a run finishing later would.
                std::thread([&, l, ticket, mutex] {
                    std::lock_guard<std::mutex> wait(*mutex);   // the ticket is set
                    int now = ++inside;
                    int prev = maxInside.load();
                    while (now > prev && !maxInside.compare_exchange_weak(prev, now)) {}
                    --inside;
                    l->release(*ticket);
                    ++done;
                }).detach();
            });
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (done < kTotal && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK_EQ(done.load(), kTotal);
        CHECK_EQ(maxInside.load(), 1);
        CHECK_EQ(locks->stats().held, 0u);
    }
}

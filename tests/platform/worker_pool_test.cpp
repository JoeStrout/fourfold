#include "platform/worker_pool.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "doctest.h"
#include "platform/event_loop.h"

using namespace fourfold;

TEST_CASE("WorkerPool: runs tasks off the calling thread, and reports back through a loop") {
    auto loop = EventLoop::create();
    REQUIRE(loop.ok());
    WorkerPool pool(2);
    CHECK_EQ(pool.threadCount(), 2u);

    std::thread::id loopThread = std::this_thread::get_id();
    std::atomic<int> ranOffLoop{0};
    int reported = 0;
    const int kTasks = 20;
    for (int i = 0; i < kTasks; i++) {
        pool.submit([&] {
            if (std::this_thread::get_id() != loopThread) ranOffLoop++;
            loop.value()->post([&] {
                CHECK(std::this_thread::get_id() == loopThread);
                if (++reported == kTasks) loop.value()->stop();
            });
        });
    }
    loop.value()->run();
    CHECK_EQ(ranOffLoop.load(), kTasks);
    CHECK_EQ(reported, kTasks);
}

TEST_CASE("WorkerPool: work runs in parallel on several threads") {
    WorkerPool pool(4);
    std::atomic<int> inside{0}, maxInside{0}, done{0};
    for (int i = 0; i < 4; i++) {
        pool.submit([&] {
            int now = ++inside;
            int prev = maxInside.load();
            while (now > prev && !maxInside.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            inside--;
            done++;
        });
    }
    while (done < 4) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(maxInside.load() > 1);
}

TEST_CASE("WorkerPool: destroying it drops tasks that haven't started") {
    std::atomic<int> ran{0};
    {
        WorkerPool pool(1);
        pool.submit([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ran++;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));   // let the first start
        for (int i = 0; i < 10; i++) pool.submit([&] { ran++; });
    }
    CHECK_EQ(ran.load(), 1);
}

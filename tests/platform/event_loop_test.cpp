#include "platform/event_loop.h"

#include <thread>
#include <vector>

#include "doctest.h"

using namespace fourfold;
using std::chrono::milliseconds;

namespace {

std::unique_ptr<EventLoop> makeLoop() {
    auto loop = EventLoop::create();
    REQUIRE(loop.ok());
    return std::move(loop).value();
}

std::pair<Fd, Fd> makePair() {
    auto pair = makeSocketPair();
    REQUIRE(pair.ok());
    return std::move(pair).value();
}

}  // namespace

TEST_CASE("EventLoop: posted tasks run on the loop, even from another thread") {
    auto loop = makeLoop();
    std::thread::id loopThread = std::this_thread::get_id();
    std::thread::id ranOn;
    std::thread poster([&] {
        loop->post([&] {
            ranOn = std::this_thread::get_id();
            loop->stop();
        });
    });
    loop->run();   // returns once the posted task calls stop()
    poster.join();
    CHECK(ranOn == loopThread);
}

TEST_CASE("EventLoop: timers run in deadline order, and can be cancelled") {
    auto loop = makeLoop();
    std::vector<int> order;
    loop->addTimer(milliseconds(20), [&] { order.push_back(2); });
    loop->addTimer(milliseconds(1), [&] { order.push_back(1); });
    auto cancelled = loop->addTimer(milliseconds(5), [&] { order.push_back(99); });
    loop->addTimer(milliseconds(30), [&] { loop->stop(); });
    loop->cancelTimer(cancelled);
    loop->run();
    CHECK(order == std::vector<int>{1, 2});
}

TEST_CASE("EventLoop: a timer can add another timer") {
    auto loop = makeLoop();
    int fired = 0;
    loop->addTimer(milliseconds(1), [&] {
        fired++;
        loop->addTimer(milliseconds(1), [&] {
            fired++;
            loop->stop();
        });
    });
    loop->run();
    CHECK_EQ(fired, 2);
}

TEST_CASE("EventLoop: a watched socket reports readable, until unwatched") {
    auto loop = makeLoop();
    auto pair = makePair();
    Fd& a = pair.first;
    Fd& b = pair.second;
    int calls = 0;
    std::string received;
    REQUIRE(loop->watch(a.get(), io::kReadable, [&](uint32_t events) {
        calls++;
        CHECK((events & io::kReadable) != 0);
        char buffer[64];
        auto n = readSome(a.get(), buffer, sizeof buffer);
        if (n) received.append(buffer, n.value());
        loop->unwatch(a.get());   // unwatching from inside the callback is safe
    }).ok());

    loop->runOnce(milliseconds(0));
    CHECK_EQ(calls, 0);   // nothing written yet

    REQUIRE(writeSome(b.get(), "hi", 2).ok());
    loop->runOnce(milliseconds(100));
    CHECK_EQ(calls, 1);
    CHECK_EQ(received, "hi");

    REQUIRE(writeSome(b.get(), "more", 4).ok());
    loop->runOnce(milliseconds(10));
    CHECK_EQ(calls, 1);   // unwatched
}

TEST_CASE("EventLoop: setInterest switches between reading and writing") {
    auto loop = makeLoop();
    auto pair = makePair();
    Fd& a = pair.first;
    uint32_t seen = 0;
    REQUIRE(loop->watch(a.get(), 0, [&](uint32_t events) { seen |= events; }).ok());
    loop->runOnce(milliseconds(0));
    CHECK_EQ(seen, 0u);   // interest 0: paused

    REQUIRE(loop->setInterest(a.get(), io::kWritable).ok());
    loop->runOnce(milliseconds(100));
    CHECK_EQ(seen, io::kWritable);   // an empty socket is writable at once

    CHECK_FALSE(loop->setInterest(12345, io::kReadable).ok());   // not watched
}

TEST_CASE("EventLoop: re-watching an fd replaces the old callback") {
    auto loop = makeLoop();
    auto pair = makePair();
    Fd& a = pair.first;
    Fd& b = pair.second;
    int oldCalls = 0, newCalls = 0;
    REQUIRE(loop->watch(a.get(), io::kReadable, [&](uint32_t) { oldCalls++; }).ok());
    REQUIRE(loop->watch(a.get(), io::kReadable, [&](uint32_t) {
        newCalls++;
        loop->unwatch(a.get());
    }).ok());
    REQUIRE(writeSome(b.get(), "x", 1).ok());
    loop->runOnce(milliseconds(100));
    CHECK_EQ(oldCalls, 0);
    CHECK_EQ(newCalls, 1);
}

TEST_CASE("EventLoop: runOnce returns after maxWait when nothing happens") {
    auto loop = makeLoop();
    auto start = EventLoop::Clock::now();
    loop->runOnce(milliseconds(20));
    auto elapsed = EventLoop::Clock::now() - start;
    CHECK(elapsed >= milliseconds(15));
    CHECK(elapsed < milliseconds(1000));
}

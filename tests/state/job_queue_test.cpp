// JobQueue (M9.1): queued, running and dead jobs, keys, retries, jobs left
// running by a stopped engine, and the schedule's state.
#include "state/job_queue.h"

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

struct Fixture {
    TempDir dir;
    ManualClock clock{1'800'000'000'000};
    std::unique_ptr<JobQueue> queue;

    Fixture() { queue = JobQueue::open(dir.at("state.db"), clock).value(); }

    NewJob job(const std::string& name, int64_t delayMs = 0, std::optional<std::string> key = std::nullopt) {
        NewJob j;
        j.name = name;
        j.argsJson = "{\"n\":1}";
        j.key = std::move(key);
        j.runAtMs = delayMs ? clock.nowMillis() + delayMs : 0;
        j.source = "test";
        return j;
    }

    std::optional<QueuedJob> claim() {
        auto c = queue->claim();
        REQUIRE(c);
        return c.value();
    }
};

}  // namespace

TEST_CASE_FIXTURE(Fixture, "jobs are claimed in the order they're due, once each") {
    REQUIRE(queue->enqueue({job("a"), job("later", 60'000), job("b")}).value() == 3);
    auto first = claim();
    REQUIRE(first);
    CHECK(first->name == "a");
    CHECK(first->state == QueuedJob::State::Running);
    CHECK(first->attempts == 1);
    CHECK(first->argsJson == "{\"n\":1}");
    CHECK(first->source == "test");
    auto second = claim();
    REQUIRE(second);
    CHECK(second->name == "b");
    CHECK(!claim());   // "later" isn't due
    CHECK(queue->nextDue().value() == clock.nowMillis() + 60'000);
    clock.advanceSeconds(60);
    auto third = claim();
    REQUIRE(third);
    CHECK(third->name == "later");

    REQUIRE(queue->succeed(first->id));
    auto runs = queue->recentRuns().value();
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].name == "a");
    CHECK(runs[0].outcome == "ok");
    CHECK(queue->list().value().size() == 2);   // b and later, running
}

TEST_CASE_FIXTURE(Fixture, "a key is unique while its job is queued") {
    CHECK(queue->enqueue({job("digest", 0, "digest:u1")}).value() == 1);
    CHECK(queue->enqueue({job("digest", 0, "digest:u1")}).value() == 0);
    CHECK(queue->enqueue({job("digest", 0, "digest:u2"), job("digest", 0, "digest:u1")}).value() == 1);
    CHECK(queue->pending("digest:u1").value());
    // Once it's running, the key may be queued again.
    auto running = claim();
    REQUIRE(running);
    CHECK(running->key == std::optional<std::string>("digest:u1"));
    CHECK(queue->pending("digest:u1").value());
    CHECK(queue->enqueue({job("digest", 0, "digest:u1")}).value() == 1);
    // If the running one fails, the queued one with its key takes its place.
    REQUIRE(queue->fail(running->id, "boom", clock.nowMillis() + 1000));
    size_t withKey = 0;
    std::vector<QueuedJob> listed = queue->list().value();
    for (const QueuedJob& j : listed) withKey += j.key == std::optional<std::string>("digest:u1");
    CHECK(withKey == 1);
    CHECK(queue->recentRuns().value()[0].outcome == "dropped");
}

TEST_CASE_FIXTURE(Fixture, "failures: retried when told, dead otherwise; dead jobs retried or discarded") {
    REQUIRE(queue->enqueue({job("flaky")}));
    auto j = claim();
    REQUIRE(j);
    REQUIRE(queue->fail(j->id, "first error", clock.nowMillis() + 15'000));
    CHECK(!claim());
    clock.advanceSeconds(15);
    j = claim();
    REQUIRE(j);
    CHECK(j->attempts == 2);
    CHECK(j->lastError == "first error");
    REQUIRE(queue->fail(j->id, "second error", std::nullopt));
    auto list = queue->list().value();
    REQUIRE(list.size() == 1);
    CHECK(list[0].state == QueuedJob::State::Dead);
    CHECK(list[0].lastError == "second error");
    CHECK(!claim());
    auto runs = queue->recentRuns().value();
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].outcome == "dead");
    CHECK(runs[1].outcome == "retry");

    REQUIRE(queue->retry(list[0].id));
    j = claim();
    REQUIRE(j);
    CHECK(j->attempts == 1);   // from zero again
    REQUIRE(queue->fail(j->id, "again", std::nullopt));
    REQUIRE(queue->discard(j->id));
    CHECK(queue->list().value().empty());
}

TEST_CASE_FIXTURE(Fixture, "jobs left running by a stopped engine are queued again") {
    REQUIRE(queue->enqueue({job("long"), job("keyed", 0, "k")}));
    REQUIRE(claim());
    REQUIRE(claim());
    // A newer job with the same key arrived while "keyed" ran.
    REQUIRE(queue->enqueue({job("keyed", 0, "k")}).value() == 1);
    // The engine stops; another opens the same store.
    queue.reset();
    queue = JobQueue::open(dir.at("state.db"), clock).value();
    CHECK(queue->requeueRunning().value() == 1);   // "long"; the old "keyed" gives way
    auto list = queue->list().value();
    REQUIRE(list.size() == 2);
    for (const QueuedJob& j : list) {
        CHECK(j.state == QueuedJob::State::Queued);
        if (j.name == "long") {
            CHECK(j.attempts == 1);
            CHECK(j.lastError.find("interrupted") != std::string::npos);
        }
    }
    auto again = claim();
    REQUIRE(again);
    CHECK(again->attempts == 2);
    auto names = queue->pendingNames().value();
    CHECK(names == std::vector<std::string>{"keyed", "long"});
}

TEST_CASE_FIXTURE(Fixture, "the schedule's state") {
    CHECK(queue->schedules().value().empty());
    REQUIRE(queue->saveSchedule({"cleanup", "daily 03:00 @ UTC", 0, 5000}));
    REQUIRE(queue->saveSchedule({"poll", "every 15m @ UTC", 100, 1000}));
    REQUIRE(queue->saveSchedule({"cleanup", "daily 03:00 @ UTC", 5000, 9000}));
    auto states = queue->schedules().value();
    REQUIRE(states.size() == 2);
    CHECK(states["cleanup"].lastRunMs == 5000);
    CHECK(states["cleanup"].nextRunMs == 9000);
    REQUIRE(queue->removeSchedule("poll"));
    CHECK(queue->schedules().value().size() == 1);
}

TEST_CASE_FIXTURE(Fixture, "recent runs are trimmed") {
    for (int i = 0; i < 230; i++) {
        REQUIRE(queue->enqueue({job("tick")}));
        auto j = claim();
        REQUIRE(j);
        REQUIRE(queue->succeed(j->id));
    }
    CHECK(queue->recentRuns(1000).value().size() <= 201);
    CHECK(queue->recentRuns(10).value().size() == 10);
}

#include "base/clock.h"

#include "doctest.h"

using namespace fourfold;

TEST_CASE("systemClock: plausible wall time") {
    int64_t now = systemClock().nowMillis();
    CHECK(now > 1'700'000'000'000);   // after 2023
    CHECK(systemClock().nowMillis() >= now);
}

TEST_CASE("ManualClock moves only when told") {
    ManualClock clock(1000);
    CHECK_EQ(clock.nowMillis(), 1000);
    clock.advanceSeconds(2);
    clock.advanceMillis(5);
    CHECK_EQ(clock.nowMillis(), 3005);
}

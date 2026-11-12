// Schedules (M9.1): parsing code/schedule.ms's forms, and when each is next
// due, across DST changes in a named zone.
#include "compiledapp/schedule.h"

#include <climits>

#include "doctest.h"

using namespace fourfold;

namespace {

int64_t utc(int y, int mo, int d, int h, int mi) {
    CivilTime t;
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = h;
    t.minute = mi;
    return secondsFromCivil(t);
}

Schedule parse(const std::string& text) {
    std::string error;
    auto s = parseSchedule(text, error);
    REQUIRE_MESSAGE(s, text, ": ", error);
    return *s;
}

std::string errorOf(const std::string& text) {
    std::string error;
    auto s = parseSchedule(text, error);
    CHECK_MESSAGE(!s, text);
    return error;
}

std::shared_ptr<const TimeZone> denver() {
    auto z = TimeZone::load("America/Denver");
    REQUIRE(z);
    return z.value();
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("schedules: the friendly forms and cron") {
    const TimeZone& u = *TimeZone::utc();
    Schedule every = parse("every 15m");
    CHECK(every.kind == Schedule::Kind::Every);
    CHECK(every.everySeconds == 900);
    CHECK(every.nextAfter(1000, u) == 1900);
    CHECK(every.describe() == "every 15 minutes");
    CHECK(parse("every 2 hours").everySeconds == 7200);
    CHECK(parse("every hour").everySeconds == 3600);
    CHECK(parse("every day").describe() == "every day");
    CHECK(parse("Every 90 Minutes").everySeconds == 5400);

    int64_t t = utc(2026, 11, 12, 10, 7);   // a Thursday
    CHECK(parse("hourly").nextAfter(t, u) == utc(2026, 11, 12, 11, 0));
    CHECK(parse("hourly :15").nextAfter(t, u) == utc(2026, 11, 12, 10, 15));
    CHECK(parse("daily 03:00").nextAfter(t, u) == utc(2026, 11, 13, 3, 0));
    CHECK(parse("daily at 3:00").nextAfter(t, u) == utc(2026, 11, 13, 3, 0));
    CHECK(parse("daily 10:07").nextAfter(t, u) == utc(2026, 11, 13, 10, 7));   // strictly after
    CHECK(parse("weekly mon 08:00").nextAfter(t, u) == utc(2026, 11, 16, 8, 0));
    CHECK(parse("weekly thursday 23:30").nextAfter(t, u) == utc(2026, 11, 12, 23, 30));
    CHECK(parse("monthly 1 06:00").nextAfter(t, u) == utc(2026, 12, 1, 6, 0));
    CHECK(parse("0 6 1 * *").nextAfter(t, u) == utc(2026, 12, 1, 6, 0));
    CHECK(parse("*/20 * * * *").nextAfter(t, u) == utc(2026, 11, 12, 10, 20));
    CHECK(parse("0 9 * * mon-fri").nextAfter(utc(2026, 11, 13, 10, 0), u) == utc(2026, 11, 16, 9, 0));
    CHECK(parse("30 4 * jan,jul *").nextAfter(t, u) == utc(2027, 1, 1, 4, 30));
    CHECK(parse("0 0 * * 7").nextAfter(t, u) == utc(2026, 11, 15, 0, 0));      // 7 is Sunday
    CHECK(parse("0 0 * * fri-sun").nextAfter(t, u) == utc(2026, 11, 13, 0, 0));
    // Both day fields restricted: either matches, as in cron.
    CHECK(parse("0 0 13 * 5").nextAfter(t, u) == utc(2026, 11, 13, 0, 0));    // Friday the 13th, either way
    CHECK(parse("0 0 20 * 1").nextAfter(t, u) == utc(2026, 11, 16, 0, 0));    // the Monday comes first
    // Feb 29: the next leap year.
    CHECK(parse("0 12 29 2 *").nextAfter(t, u) == utc(2028, 2, 29, 12, 0));
}

TEST_CASE("schedules: what isn't one") {
    CHECK(contains(errorOf("every 30s"), "too often"));
    CHECK(contains(errorOf("every 0m"), "interval"));
    CHECK(contains(errorOf("every fortnight"), "interval"));
    CHECK(contains(errorOf("daily 25:00"), "daily 03:00"));
    CHECK(contains(errorOf("daily 3pm"), "24-hour"));
    CHECK(contains(errorOf("weekly funday 08:00"), "weekday"));
    CHECK(contains(errorOf("monthly 31 06:00"), "1 to 28"));
    CHECK(contains(errorOf("hourly 75"), ":15"));
    CHECK(contains(errorOf("0 0 31 2 *"), "never runs"));
    CHECK(contains(errorOf("60 * * * *"), "minute"));
    CHECK(contains(errorOf("* * * * 1-x"), "weekday"));
    CHECK(contains(errorOf("5-1 * * * *"), "backward"));
    CHECK(contains(errorOf("*/0 * * * *"), "step"));
    CHECK(contains(errorOf("sometimes"), "isn't a schedule"));
    CHECK(contains(errorOf(""), "empty"));
}

TEST_CASE("schedules in America/Denver: spring forward") {
    auto zone = denver();
    // 02:30 doesn't happen on March 8: the job runs at the jump (03:00 MDT,
    // 09:00 UTC), once.
    Schedule s = parse("daily 02:30");
    int64_t first = s.nextAfter(utc(2026, 3, 7, 10, 0), *zone);
    CHECK(first == utc(2026, 3, 8, 9, 0));
    // Then 02:30 MDT the next day (08:30 UTC).
    CHECK(s.nextAfter(first, *zone) == utc(2026, 3, 9, 8, 30));
    // The day before, it was 02:30 MST (09:30 UTC).
    CHECK(s.nextAfter(utc(2026, 3, 6, 10, 0), *zone) == utc(2026, 3, 7, 9, 30));
    // 03:00 that night is the same instant as the jump: it runs once.
    Schedule three = parse("daily 03:00");
    CHECK(three.nextAfter(utc(2026, 3, 7, 12, 0), *zone) == utc(2026, 3, 8, 9, 0));
    // A daily job keeps its local time: 08:00 is 15:00 UTC, then 14:00.
    Schedule eight = parse("daily 08:00");
    CHECK(eight.nextAfter(utc(2026, 3, 7, 16, 0), *zone) == utc(2026, 3, 8, 14, 0));
    CHECK(eight.nextAfter(utc(2026, 3, 6, 16, 0), *zone) == utc(2026, 3, 7, 15, 0));
}

TEST_CASE("schedules in America/Denver: fall back") {
    auto zone = denver();
    // 01:30 happens twice on November 1: the job runs at the first (MDT).
    Schedule s = parse("daily 01:30");
    int64_t first = s.nextAfter(utc(2026, 10, 31, 12, 0), *zone);
    CHECK(first == utc(2026, 11, 1, 7, 30));
    CHECK(s.nextAfter(first, *zone) == utc(2026, 11, 2, 8, 30));   // not again at 01:30 MST
    // Hourly: the repeated hour's :30 runs once.
    Schedule hourly = parse("hourly :30");
    int64_t t = utc(2026, 11, 1, 6, 0);   // 00:00 MDT
    std::vector<int64_t> runs;
    for (int i = 0; i < 4; i++) runs.push_back(t = hourly.nextAfter(t, *zone));
    CHECK(runs[0] == utc(2026, 11, 1, 6, 30));   // 00:30 MDT
    CHECK(runs[1] == utc(2026, 11, 1, 7, 30));   // 01:30 MDT
    CHECK(runs[2] == utc(2026, 11, 1, 9, 30));   // 02:30 MST (01:30 MST skipped)
    CHECK(runs[3] == utc(2026, 11, 1, 10, 30));
    // Started during the repeated hour: the next is after it.
    CHECK(hourly.nextAfter(utc(2026, 11, 1, 8, 10), *zone) == utc(2026, 11, 1, 9, 30));
    // An interval is real time: DST doesn't stretch it.
    CHECK(parse("every 1h").nextAfter(utc(2026, 11, 1, 7, 30), *zone) == utc(2026, 11, 1, 8, 30));
}

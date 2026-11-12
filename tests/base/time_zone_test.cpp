// Time zones (M9.1): the system's zoneinfo files and POSIX rules, with the
// DST cases schedules depend on.
#include "base/time_zone.h"

#include "doctest.h"

using namespace fourfold;

namespace {

int64_t utc(int y, int mo, int d, int h, int mi, int s = 0) {
    CivilTime t;
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = h;
    t.minute = mi;
    t.second = s;
    return secondsFromCivil(t);
}

std::shared_ptr<const TimeZone> zone(const std::string& name) {
    auto z = TimeZone::load(name);
    REQUIRE_MESSAGE(z, (z ? "" : z.error().message));
    return z.value();
}

}  // namespace

TEST_CASE("civil dates: days, weekdays, and back") {
    CHECK(daysFromCivil(1970, 1, 1) == 0);
    CHECK(daysFromCivil(2000, 3, 1) == 11017);
    CivilTime t = civilFromSeconds(utc(2026, 11, 12, 15, 4, 5));
    CHECK(t.year == 2026);
    CHECK(t.month == 11);
    CHECK(t.day == 12);
    CHECK(t.hour == 15);
    CHECK(t.minute == 4);
    CHECK(t.second == 5);
    CHECK(t.weekday == 4);   // a Thursday
    CHECK(civilFromSeconds(-1).year == 1969);
    CHECK(civilFromSeconds(-1).weekday == 3);
    CHECK(daysInMonth(2024, 2) == 29);
    CHECK(daysInMonth(2100, 2) == 28);
    CHECK(civilFromSeconds(utc(2026, 3, 8, 0, 0)).weekday == 0);   // a Sunday
}

TEST_CASE("UTC, and names that can't be zones") {
    auto u = TimeZone::utc();
    CHECK(u->offsetAt(utc(2026, 7, 1, 0, 0)) == 0);
    CHECK(TimeZone::load("UTC").value()->name() == "UTC");
    for (const char* bad : {"", "../etc/passwd", "America/../x", "/etc/passwd", "a//b", ".hidden"}) {
        auto z = TimeZone::load(bad);
        REQUIRE_MESSAGE(!z, bad);
        CHECK(z.error().code == ErrorCode::Invalid);
    }
    auto missing = TimeZone::load("Mars/Olympus_Mons");
    REQUIRE(!missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(isTimeZoneName("America/Argentina/Buenos_Aires"));
    CHECK(isTimeZoneName("Etc/GMT+7"));
}

TEST_CASE("America/Denver: offsets across both DST changes in 2026") {
    auto denver = zone("America/Denver");
    // Clocks go forward at 02:00 MST on March 8 (09:00 UTC)...
    CHECK(denver->offsetAt(utc(2026, 3, 8, 8, 59, 59)) == -7 * 3600);
    CHECK(denver->offsetAt(utc(2026, 3, 8, 9, 0)) == -6 * 3600);
    // ...and back at 02:00 MDT on November 1 (08:00 UTC).
    CHECK(denver->offsetAt(utc(2026, 11, 1, 7, 59, 59)) == -6 * 3600);
    CHECK(denver->offsetAt(utc(2026, 11, 1, 8, 0)) == -7 * 3600);
    // Long after the file's last transition, its POSIX rule takes over.
    CHECK(denver->offsetAt(utc(2100, 7, 1, 12, 0)) == -6 * 3600);
    CHECK(denver->offsetAt(utc(2100, 1, 1, 12, 0)) == -7 * 3600);
    // And long before the first, the first type.
    CHECK(denver->offsetAt(utc(1850, 1, 1, 0, 0)) != 0);
}

TEST_CASE("America/Denver: local times skipped, repeated, and ordinary") {
    auto denver = zone("America/Denver");
    TimeZone::LocalKind kind;
    // 02:30 on March 8 never happens: it's the jump, 03:00 MDT.
    CHECK(denver->toUtc(utc(2026, 3, 8, 2, 30), &kind) == utc(2026, 3, 8, 9, 0));
    CHECK(kind == TimeZone::LocalKind::Skipped);
    // 01:30 on November 1 happens twice: the first (MDT).
    CHECK(denver->toUtc(utc(2026, 11, 1, 1, 30), &kind) == utc(2026, 11, 1, 7, 30));
    CHECK(kind == TimeZone::LocalKind::Repeated);
    CHECK(denver->toUtc(utc(2026, 7, 1, 12, 0), &kind) == utc(2026, 7, 1, 18, 0));
    CHECK(kind == TimeZone::LocalKind::Unique);
    CHECK(denver->toLocal(utc(2026, 1, 15, 19, 0)) == utc(2026, 1, 15, 12, 0));
}

TEST_CASE("POSIX rules: the same zone from its rule alone, and the southern hemisphere") {
    auto rule = TimeZone::fromPosixRule("Denver", "MST7MDT,M3.2.0,M11.1.0");
    REQUIRE(rule);
    auto denver = zone("America/Denver");
    for (int64_t t = utc(2026, 1, 1, 0, 0); t < utc(2027, 1, 1, 0, 0); t += 3 * 3600 + 17) {
        REQUIRE(rule.value()->offsetAt(t) == denver->offsetAt(t));
    }
    // Sydney: daylight time from October to April.
    auto sydney = TimeZone::fromPosixRule("Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3");
    REQUIRE(sydney);
    CHECK(sydney.value()->offsetAt(utc(2026, 1, 15, 0, 0)) == 11 * 3600);
    CHECK(sydney.value()->offsetAt(utc(2026, 7, 1, 0, 0)) == 10 * 3600);
    // Quoted names, and no DST.
    auto plus3 = TimeZone::fromPosixRule("x", "<+03>-3");
    REQUIRE(plus3);
    CHECK(plus3.value()->offsetAt(0) == 3 * 3600);
    CHECK(!TimeZone::fromPosixRule("x", "nonsense,"));
    CHECK(!TimeZone::fromPosixRule("x", "EST5EDT,M13.1.0,M11.1.0"));
}

TEST_CASE("TZif: bad files are refused") {
    CHECK(!TimeZone::fromTzif("x", "not a zone file at all, but long enough to have a header....."));
    CHECK(!TimeZone::fromTzif("x", "TZif2"));
}

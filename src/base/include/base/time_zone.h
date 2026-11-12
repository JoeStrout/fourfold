// TimeZone: an IANA time zone ("America/Denver"), for schedules (overview
// §2, Scheduling; decided for M9.1), read from the system's zoneinfo files.
//
// A zone is its list of transitions (from the TZif file) and, for times
// after the last one, the POSIX rule in the file's footer
// ("MST7MDT,M3.2.0,M11.1.0"), which modern "slim" zoneinfo files rely on.
//
// Times here are seconds: UTC instants since the Unix epoch, and local
// times as the same count read on the zone's clock (a local time's
// "seconds since 1970-01-01 00:00 local").
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/result.h"

namespace fourfold {

// Civil dates (proleptic Gregorian), for local times.
struct CivilTime {
    int year = 1970;
    int month = 1;    // 1..12
    int day = 1;      // 1..31
    int hour = 0;
    int minute = 0;
    int second = 0;
    int weekday = 4;  // 0 = Sunday (1970-01-01 was a Thursday)
};

// Days since 1970-01-01 for a civil date, and back.
int64_t daysFromCivil(int year, int month, int day);
CivilTime civilFromSeconds(int64_t seconds);   // with weekday
int64_t secondsFromCivil(const CivilTime& t);  // ignores weekday
int daysInMonth(int year, int month);

class TimeZone {
public:
    // UTC: no offset, ever.
    static std::shared_ptr<const TimeZone> utc();
    // A zone by IANA name ("America/Denver", "UTC"), from $TZDIR or
    // /usr/share/zoneinfo. NotFound for an unknown zone; Invalid for a name
    // that can't be one (".." and the like).
    static Result<std::shared_ptr<const TimeZone>> load(const std::string& name);
    // A zone from TZif bytes (for tests).
    static Result<std::shared_ptr<const TimeZone>> fromTzif(const std::string& name, std::string_view bytes);
    // A zone from a POSIX TZ rule alone ("EST5EDT,M3.2.0,M11.1.0").
    static Result<std::shared_ptr<const TimeZone>> fromPosixRule(const std::string& name, const std::string& rule);

    const std::string& name() const { return name_; }

    // Seconds east of UTC at the UTC instant `utc` (-25200 for MST).
    int32_t offsetAt(int64_t utc) const;

    // The UTC instant for a local time. A local time that happens twice (the
    // hour repeated when clocks go back) gives the first; one that never
    // happens (skipped when clocks go forward) gives the instant of the
    // jump. `kind` says which.
    enum class LocalKind { Unique, Repeated, Skipped };
    int64_t toUtc(int64_t local, LocalKind* kind = nullptr) const;

    // The local time at a UTC instant.
    int64_t toLocal(int64_t utc) const { return utc + offsetAt(utc); }

    struct Rule;   // private: a POSIX TZ rule

private:
    TimeZone() = default;

    std::string name_;
    std::vector<int64_t> transitions_;   // UTC instants, ascending
    std::vector<int32_t> offsets_;       // the offset from each transition on
    int32_t initialOffset_ = 0;          // before the first transition
    std::shared_ptr<const Rule> rule_;   // after the last transition; null: the last offset holds
};

// Whether `name` could be an IANA zone name: letters, digits, '/', '_',
// '-', '+', no empty or dot-led parts, at most 64 bytes.
bool isTimeZoneName(const std::string& name);

}  // namespace fourfold

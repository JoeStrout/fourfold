// Schedules for jobs (overview §2, Scheduling): code/schedule.ms's values,
// parsed at compile time.
//
//   "every 15m", "every 2 hours", "every day"   an interval since the last run
//   "hourly", "hourly :15"                       at a minute of every hour
//   "daily 03:00"                                at a time each day
//   "weekly mon 08:00"                           on a weekday
//   "monthly 1 06:00"                            on a day of the month
//   "0 6 1 * *"                                  cron: minute hour day month weekday
//
// Times are in the app's time zone (config.jobs.timeZone). A time a DST
// change skips runs at the jump; a time that happens twice runs once (the
// first time). An interval is real time, so DST doesn't change it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "base/time_zone.h"

namespace fourfold {

struct Schedule {
    enum class Kind { Every, Cron };

    Kind kind = Kind::Cron;
    std::string text;            // as written
    int64_t everySeconds = 0;    // Every

    // Cron: which minutes (bits 0-59), hours (0-23), days of the month
    // (1-31), months (1-12) and weekdays (0-6, Sunday 0) match. A day
    // matches if its day of the month and its weekday do; but if both are
    // restricted (neither is "*"), either will do, as in cron.
    uint64_t minutes = 0;
    uint32_t hours = 0;
    uint32_t days = 0;
    uint16_t months = 0;
    uint8_t weekdays = 0;
    bool anyDay = true;
    bool anyWeekday = true;

    // The first time it's due after `after` (UTC seconds, exclusive), in
    // `zone`: for Every, `after` + the interval. INT64_MAX if never (within
    // a few years).
    int64_t nextAfter(int64_t after, const TimeZone& zone) const;

    // "every 15 minutes", "at 03:00 every day", or the cron text.
    std::string describe() const;
};

// A schedule as written; nullopt, with `error` set, if it isn't one.
std::optional<Schedule> parseSchedule(const std::string& text, std::string& error);

}  // namespace fourfold

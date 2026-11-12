#include "compiledapp/schedule.h"

#include <cctype>
#include <climits>
#include <sstream>
#include <vector>

namespace fourfold {

namespace {

constexpr int64_t kDay = 86400;
// How far ahead to look for a cron match (a spec like "0 0 31 2 *" never
// matches).
constexpr int64_t kSearchDays = 4 * 366 + 1;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

bool allDigits(const std::string& s) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

const char* const kWeekdays[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
const char* const kWeekdayNames[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
const char* const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

int weekdayFrom(const std::string& w) {
    for (int i = 0; i < 7; i++) {
        if (w == kWeekdays[i] || w == kWeekdayNames[i]) return i;
    }
    return -1;
}

// "03:00", "3:00", "15:30" → minutes since midnight; -1 if it isn't a time.
int timeOfDay(const std::string& s) {
    size_t colon = s.find(':');
    if (colon == std::string::npos) return -1;
    std::string h = s.substr(0, colon), m = s.substr(colon + 1);
    if (!allDigits(h) || h.size() > 2 || !allDigits(m) || m.size() != 2) return -1;
    int hour = std::stoi(h), minute = std::stoi(m);
    if (hour > 23 || minute > 59) return -1;
    return hour * 60 + minute;
}

// One cron field: "*", "*/5", "1-5", "1,15", "mon-fri", "0-30/10".
bool cronField(const std::string& field, int lo, int hi, const char* const* names, uint64_t& bits, bool& any,
               std::string& error, const char* what) {
    bits = 0;
    any = field == "*";
    std::string text = lower(field);
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        int step = 1;
        size_t slash = part.find('/');
        if (slash != std::string::npos) {
            std::string s = part.substr(slash + 1);
            if (!allDigits(s) || std::stoi(s) == 0) {
                error = std::string("the ") + what + " field's step \"" + s + "\" isn't a positive number";
                return false;
            }
            step = std::stoi(s);
            part = part.substr(0, slash);
        }
        auto value = [&](const std::string& v, int& out) {
            if (allDigits(v)) {
                out = std::stoi(v);
            } else {
                out = -1;
                int count = names == kWeekdays ? 7 : names == kMonths ? 12 : 0;
                for (int i = 0; i < count; i++) {
                    if (v == names[i]) out = lo + i;
                }
                if (out < 0) return false;
            }
            // Weekday 7 is Sunday too.
            if (names == kWeekdays && out == 7) out = 0;
            return out >= lo && out <= hi;
        };
        int a = lo, b = hi;
        if (part != "*") {
            size_t dash = part.find('-');
            if (dash == std::string::npos) {
                if (!value(part, a)) {
                    error = "\"" + part + "\" isn't a valid " + what;
                    return false;
                }
                b = slash != std::string::npos ? hi : a;
            } else if (!value(part.substr(0, dash), a) || !value(part.substr(dash + 1), b)) {
                error = "\"" + part + "\" isn't a valid range of " + what + "s";
                return false;
            }
            if (b < a && !(names == kWeekdays && b == 0)) {
                error = "the range \"" + part + "\" goes backward";
                return false;
            }
            if (names == kWeekdays && b == 0 && a > 0) b = 6, bits |= 1;   // "fri-sun"
        }
        for (int v = a; v <= b; v += step) bits |= uint64_t{1} << v;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (bits == 0) {
        error = std::string("the ") + what + " field matches nothing";
        return false;
    }
    return true;
}

bool parseCron(const std::vector<std::string>& f, Schedule& s, std::string& error) {
    uint64_t bits = 0;
    bool any = false;
    if (!cronField(f[0], 0, 59, nullptr, bits, any, error, "minute")) return false;
    s.minutes = bits;
    if (!cronField(f[1], 0, 23, nullptr, bits, any, error, "hour")) return false;
    s.hours = static_cast<uint32_t>(bits);
    if (!cronField(f[2], 1, 31, nullptr, bits, any, error, "day")) return false;
    s.days = static_cast<uint32_t>(bits);
    s.anyDay = any;
    if (!cronField(f[3], 1, 12, kMonths, bits, any, error, "month")) return false;
    s.months = static_cast<uint16_t>(bits);
    if (!cronField(f[4], 0, 7, kWeekdays, bits, any, error, "weekday")) return false;
    if (bits & (uint64_t{1} << 7)) bits = (bits | 1) & 0x7F;
    s.weekdays = static_cast<uint8_t>(bits);
    s.anyWeekday = any;
    return true;
}

void setTime(Schedule& s, int minutes) {
    s.minutes = uint64_t{1} << (minutes % 60);
    s.hours = uint32_t{1} << (minutes / 60);
    s.days = 0xFFFFFFFE;   // 1-31
    s.months = 0x1FFE;     // 1-12
    s.weekdays = 0x7F;
}

}  // namespace

std::optional<Schedule> parseSchedule(const std::string& text, std::string& error) {
    Schedule s;
    s.text = text;
    std::vector<std::string> w = words(lower(text));
    const char* const kHelp =
        "; write \"every 15m\", \"hourly :15\", \"daily 03:00\", \"weekly mon 08:00\", \"monthly 1 06:00\", or "
        "cron's five fields (\"0 6 * * 1-5\")";
    if (w.empty()) {
        error = "the schedule is empty" + std::string(kHelp);
        return std::nullopt;
    }
    if (w.size() >= 2 && w[1] == "at") w.erase(w.begin() + 1);   // "daily at 03:00"
    if (w[0] == "every") {
        s.kind = Schedule::Kind::Every;
        int64_t n = 1;
        std::string unit;
        if (w.size() == 2) {
            std::string a = w[1];
            size_t digits = 0;
            while (digits < a.size() && std::isdigit(static_cast<unsigned char>(a[digits]))) digits++;
            if (digits > 0) {
                if (digits > 6) {
                    error = "\"" + text + "\" is too long an interval";
                    return std::nullopt;
                }
                n = std::stoll(a.substr(0, digits));
                unit = a.substr(digits);
            } else {
                unit = a;
            }
        } else if (w.size() == 3 && allDigits(w[1]) && w[1].size() <= 6) {
            n = std::stoll(w[1]);
            unit = w[2];
        } else {
            error = "\"" + text + "\" isn't a schedule" + kHelp;
            return std::nullopt;
        }
        int64_t seconds = 0;
        if (unit == "s" || unit == "sec" || unit == "second" || unit == "seconds") seconds = 1;
        if (unit == "m" || unit == "min" || unit == "minute" || unit == "minutes") seconds = 60;
        if (unit == "h" || unit == "hour" || unit == "hours") seconds = 3600;
        if (unit == "d" || unit == "day" || unit == "days") seconds = kDay;
        if (seconds == 0 || n <= 0) {
            error = "\"" + text + "\" isn't an interval like \"every 15m\", \"every 2 hours\" or \"every day\"";
            return std::nullopt;
        }
        s.everySeconds = n * seconds;
        if (s.everySeconds < 60) {
            error = "\"" + text + "\" is too often: the shortest interval is a minute";
            return std::nullopt;
        }
        if (s.everySeconds > 366 * kDay) {
            error = "\"" + text + "\" is too long an interval (at most a year)";
            return std::nullopt;
        }
        return s;
    }
    if (w[0] == "hourly") {
        int minute = 0;
        if (w.size() == 2) {
            std::string m = w[1];
            if (!m.empty() && m[0] == ':') m = m.substr(1);
            if (!allDigits(m) || m.size() != 2 || std::stoi(m) > 59) {
                error = "\"" + text + "\": write the minute like \"hourly :15\"";
                return std::nullopt;
            }
            minute = std::stoi(m);
        } else if (w.size() != 1) {
            error = "\"" + text + "\" isn't a schedule" + std::string(kHelp);
            return std::nullopt;
        }
        setTime(s, minute);
        s.hours = 0xFFFFFF;
        return s;
    }
    if (w[0] == "daily" || w[0] == "weekly" || w[0] == "monthly") {
        size_t want = w[0] == "daily" ? 2 : 3;
        int minutes = w.size() == want ? timeOfDay(w.back()) : -1;
        if (minutes < 0) {
            error = "\"" + text + "\": write it like " +
                    (w[0] == "daily" ? std::string("\"daily 03:00\"") : w[0] == "weekly" ? "\"weekly mon 08:00\""
                                                                                          : "\"monthly 1 06:00\"") +
                    " (a 24-hour time)";
            return std::nullopt;
        }
        setTime(s, minutes);
        if (w[0] == "weekly") {
            int day = weekdayFrom(w[1]);
            if (day < 0) {
                error = "\"" + w[1] + "\" isn't a weekday (mon, tue, wed, thu, fri, sat or sun)";
                return std::nullopt;
            }
            s.weekdays = static_cast<uint8_t>(1 << day);
            s.anyWeekday = false;
        }
        if (w[0] == "monthly") {
            if (!allDigits(w[1]) || std::stoi(w[1]) < 1 || std::stoi(w[1]) > 28) {
                error = "\"" + w[1] + "\" isn't a day of the month from 1 to 28 (for later days, use cron, which "
                                       "skips months without them)";
                return std::nullopt;
            }
            s.days = uint32_t{1} << std::stoi(w[1]);
            s.anyDay = false;
        }
        return s;
    }
    if (w.size() == 5) {
        if (!parseCron(w, s, error)) {
            error = "\"" + text + "\": " + error;
            return std::nullopt;
        }
        if (s.nextAfter(0, *TimeZone::utc()) == INT64_MAX) {
            error = "\"" + text + "\" never runs (no such day)";
            return std::nullopt;
        }
        return s;
    }
    error = "\"" + text + "\" isn't a schedule" + std::string(kHelp);
    return std::nullopt;
}

int64_t Schedule::nextAfter(int64_t after, const TimeZone& zone) const {
    if (kind == Kind::Every) return after + everySeconds;
    int64_t local = zone.toLocal(after);
    // The next whole minute, on the zone's clock.
    int64_t start = local - (((local % 60) + 60) % 60) + 60;
    int64_t firstDay = start >= 0 ? start / kDay : -((-start + kDay - 1) / kDay);
    for (int64_t d = firstDay; d < firstDay + kSearchDays; d++) {
        CivilTime t = civilFromSeconds(d * kDay);
        if (!(months & (1u << t.month))) continue;
        bool dayOk = (days >> t.day) & 1, weekdayOk = (weekdays >> t.weekday) & 1;
        bool match = (!anyDay && !anyWeekday) ? (dayOk || weekdayOk) : (dayOk && weekdayOk);
        if (!match) continue;
        for (int h = 0; h < 24; h++) {
            if (!((hours >> h) & 1)) continue;
            int64_t hourStart = d * kDay + h * 3600;
            if (hourStart + 3600 <= start) continue;
            for (int m = 0; m < 60; m++) {
                if (!((minutes >> m) & 1)) continue;
                int64_t candidate = hourStart + m * 60;
                if (candidate < start) continue;
                int64_t utc = zone.toUtc(candidate);
                if (utc > after) return utc;
            }
        }
    }
    return INT64_MAX;
}

std::string Schedule::describe() const {
    if (kind == Kind::Every) {
        int64_t s = everySeconds;
        if (s % kDay == 0) return s == kDay ? "every day" : "every " + std::to_string(s / kDay) + " days";
        if (s % 3600 == 0) return s == 3600 ? "every hour" : "every " + std::to_string(s / 3600) + " hours";
        if (s % 60 == 0) return s == 60 ? "every minute" : "every " + std::to_string(s / 60) + " minutes";
        return "every " + std::to_string(s) + " seconds";
    }
    return text;
}

}  // namespace fourfold

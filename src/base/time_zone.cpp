#include "base/time_zone.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>

namespace fourfold {

namespace {

constexpr int64_t kDay = 86400;

int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

bool isLeap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

}  // namespace

int64_t daysFromCivil(int year, int month, int day) {
    int64_t y = year - (month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int daysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && isLeap(year) ? 29 : kDays[month - 1];
}

CivilTime civilFromSeconds(int64_t seconds) {
    int64_t days = floorDiv(seconds, kDay);
    int64_t rest = seconds - days * kDay;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    CivilTime t;
    t.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    t.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    t.year = static_cast<int>(y + (t.month <= 2 ? 1 : 0));
    t.hour = static_cast<int>(rest / 3600);
    t.minute = static_cast<int>(rest / 60 % 60);
    t.second = static_cast<int>(rest % 60);
    t.weekday = static_cast<int>(((days % 7) + 11) % 7);   // 1970-01-01 (day 0) was a Thursday (4)
    return t;
}

int64_t secondsFromCivil(const CivilTime& t) {
    return daysFromCivil(t.year, t.month, t.day) * kDay + t.hour * 3600 + t.minute * 60 + t.second;
}

// --- POSIX TZ rules ---

struct TimeZone::Rule {
    struct When {
        enum class Kind { Julian1, Julian0, MonthWeekDay } kind = Kind::MonthWeekDay;
        int day = 0;              // Julian1: 1..365 (no Feb 29); Julian0: 0..365
        int month = 0, week = 0, weekday = 0;   // MonthWeekDay: Mm.w.d
        int64_t time = 7200;      // seconds after local midnight (may be negative or past 24h)
    };
    int32_t stdOffset = 0;   // seconds east of UTC
    int32_t dstOffset = 0;
    bool hasDst = false;
    When start, end;

    // The local midnight (days since the epoch) a When falls on, in `year`.
    static int64_t dayOf(const When& w, int year) {
        switch (w.kind) {
            case When::Kind::Julian1: {
                int d = w.day;   // Feb 29 is never counted
                int64_t jan1 = daysFromCivil(year, 1, 1);
                return jan1 + d - 1 + (isLeap(year) && d >= 60 ? 1 : 0);
            }
            case When::Kind::Julian0:
                return daysFromCivil(year, 1, 1) + w.day;
            case When::Kind::MonthWeekDay: {
                int64_t first = daysFromCivil(year, w.month, 1);
                int firstWeekday = static_cast<int>(((first % 7) + 11) % 7);
                int64_t day = first + ((w.weekday - firstWeekday + 7) % 7) + 7 * (w.week - 1);
                if (w.week == 5) {
                    int64_t last = first + daysInMonth(year, w.month) - 1;
                    while (day > last) day -= 7;
                }
                return day;
            }
        }
        return 0;
    }

    int32_t offsetAt(int64_t utc) const {
        if (!hasDst) return stdOffset;
        int year = civilFromSeconds(utc + stdOffset).year;
        // The start is written in standard time, the end in daylight time.
        int64_t startUtc = dayOf(start, year) * kDay + start.time - stdOffset;
        int64_t endUtc = dayOf(end, year) * kDay + end.time - dstOffset;
        bool dst = startUtc < endUtc ? (utc >= startUtc && utc < endUtc) : !(utc >= endUtc && utc < startUtc);
        return dst ? dstOffset : stdOffset;
    }
};

namespace {

// A small reader over a POSIX TZ string.
struct RuleParser {
    const std::string& s;
    size_t i = 0;

    bool done() const { return i >= s.size(); }
    char peek() const { return done() ? '\0' : s[i]; }

    bool name() {
        if (peek() == '<') {
            size_t close = s.find('>', i);
            if (close == std::string::npos) return false;
            i = close + 1;
            return true;
        }
        size_t start = i;
        while (!done() && std::isalpha(static_cast<unsigned char>(s[i]))) i++;
        return i - start >= 3;
    }

    // [+-]hh[:mm[:ss]] as seconds; `maxHours` bounds hh.
    bool hms(int64_t& out, int maxHours) {
        int sign = 1;
        if (peek() == '+' || peek() == '-') sign = s[i++] == '-' ? -1 : 1;
        int64_t parts[3] = {0, 0, 0};
        for (int p = 0; p < 3; p++) {
            if (p > 0) {
                if (peek() != ':') break;
                i++;
            }
            size_t start = i;
            int64_t n = 0;
            while (!done() && std::isdigit(static_cast<unsigned char>(s[i])) && i - start < 3) n = n * 10 + (s[i++] - '0');
            if (i == start) return false;
            parts[p] = n;
        }
        if (parts[0] > maxHours || parts[1] > 59 || parts[2] > 59) return false;
        out = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
        return true;
    }

    bool number(int& out, int lo, int hi) {
        size_t start = i;
        int n = 0;
        while (!done() && std::isdigit(static_cast<unsigned char>(s[i])) && i - start < 4) n = n * 10 + (s[i++] - '0');
        if (i == start || n < lo || n > hi) return false;
        out = n;
        return true;
    }

    bool when(TimeZone::Rule::When& w) {
        using Kind = TimeZone::Rule::When::Kind;
        if (peek() == 'J') {
            i++;
            w.kind = Kind::Julian1;
            if (!number(w.day, 1, 365)) return false;
        } else if (peek() == 'M') {
            i++;
            w.kind = Kind::MonthWeekDay;
            if (!number(w.month, 1, 12) || peek() != '.') return false;
            i++;
            if (!number(w.week, 1, 5) || peek() != '.') return false;
            i++;
            if (!number(w.weekday, 0, 6)) return false;
        } else {
            w.kind = Kind::Julian0;
            if (!number(w.day, 0, 365)) return false;
        }
        w.time = 7200;
        if (peek() == '/') {
            i++;
            if (!hms(w.time, 167)) return false;
        }
        return true;
    }
};

std::shared_ptr<const TimeZone::Rule> parseRule(const std::string& text) {
    auto rule = std::make_shared<TimeZone::Rule>();
    RuleParser p{text};
    int64_t stdWest = 0;
    if (!p.name() || !p.hms(stdWest, 24)) return nullptr;
    rule->stdOffset = static_cast<int32_t>(-stdWest);
    if (p.done()) return rule;
    if (!p.name()) return nullptr;
    rule->hasDst = true;
    rule->dstOffset = rule->stdOffset + 3600;
    if (p.peek() != ',' && !p.done()) {
        int64_t dstWest = 0;
        if (!p.hms(dstWest, 24)) return nullptr;
        rule->dstOffset = static_cast<int32_t>(-dstWest);
    }
    if (p.done()) {
        // No dates: the US rules, as POSIX leaves it to the implementation.
        rule->start = {TimeZone::Rule::When::Kind::MonthWeekDay, 0, 3, 2, 0, 7200};
        rule->end = {TimeZone::Rule::When::Kind::MonthWeekDay, 0, 11, 1, 0, 7200};
        return rule;
    }
    if (p.peek() != ',') return nullptr;
    p.i++;
    if (!p.when(rule->start) || p.peek() != ',') return nullptr;
    p.i++;
    if (!p.when(rule->end) || !p.done()) return nullptr;
    return rule;
}

uint32_t be32(std::string_view b, size_t at) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(b[at])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(b[at + 3]));
}

int64_t be64(std::string_view b, size_t at) {
    return static_cast<int64_t>((static_cast<uint64_t>(be32(b, at)) << 32) | be32(b, at + 4));
}

}  // namespace

bool isTimeZoneName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part[0] == '.' || part[0] == '-') return false;
        for (char c : part) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '+')) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::shared_ptr<const TimeZone> TimeZone::utc() {
    static const std::shared_ptr<const TimeZone> zone = [] {
        std::shared_ptr<TimeZone> z(new TimeZone());
        z->name_ = "UTC";
        return z;
    }();
    return zone;
}

Result<std::shared_ptr<const TimeZone>> TimeZone::fromPosixRule(const std::string& name, const std::string& rule) {
    auto parsed = parseRule(rule);
    if (!parsed) return makeError(ErrorCode::Invalid, "\"" + rule + "\" isn't a POSIX time zone rule");
    std::shared_ptr<TimeZone> z(new TimeZone());
    z->name_ = name;
    z->rule_ = std::move(parsed);
    z->initialOffset_ = z->rule_->stdOffset;
    return std::shared_ptr<const TimeZone>(std::move(z));
}

Result<std::shared_ptr<const TimeZone>> TimeZone::fromTzif(const std::string& name, std::string_view b) {
    auto invalid = [&](const std::string& why) {
        return makeError(ErrorCode::Invalid, "the zoneinfo file for " + name + " " + why);
    };
    if (b.size() < 44 || b.substr(0, 4) != "TZif") return invalid("isn't a TZif file");
    char version = b[4];
    auto counts = [&](size_t at, uint32_t c[6]) {
        for (int k = 0; k < 6; k++) c[k] = be32(b, at + 20 + 4 * k);
    };
    uint32_t c[6];   // isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt
    counts(0, c);
    auto blockSize = [&](const uint32_t n[6], size_t timeSize) {
        return static_cast<size_t>(n[3]) * timeSize + n[3] + static_cast<size_t>(n[4]) * 6 + n[5] +
               static_cast<size_t>(n[2]) * (timeSize + 4) + n[1] + n[0];
    };
    size_t at = 44;
    size_t timeSize = 4;
    if (version >= '2') {
        // Skip the 32-bit block; read the 64-bit one after it.
        at += blockSize(c, 4);
        if (b.size() < at + 44 || b.substr(at, 4) != "TZif") return invalid("is truncated");
        counts(at, c);
        at += 44;
        timeSize = 8;
    }
    if (b.size() < at + blockSize(c, timeSize)) return invalid("is truncated");
    uint32_t timecnt = c[3], typecnt = c[4];
    if (typecnt == 0) return invalid("has no time types");
    std::shared_ptr<TimeZone> z(new TimeZone());
    z->name_ = name;
    std::vector<int64_t> times(timecnt);
    for (uint32_t k = 0; k < timecnt; k++) {
        times[k] = timeSize == 8 ? be64(b, at + k * 8) : static_cast<int32_t>(be32(b, at + k * 4));
    }
    size_t typeIndexAt = at + timecnt * timeSize;
    size_t typesAt = typeIndexAt + timecnt;
    std::vector<int32_t> typeOffsets(typecnt);
    for (uint32_t k = 0; k < typecnt; k++) typeOffsets[k] = static_cast<int32_t>(be32(b, typesAt + k * 6));
    for (uint32_t k = 0; k < timecnt; k++) {
        unsigned idx = static_cast<unsigned char>(b[typeIndexAt + k]);
        if (idx >= typecnt) return invalid("has a bad time type");
        if (!z->transitions_.empty() && times[k] <= z->transitions_.back()) continue;
        z->transitions_.push_back(times[k]);
        z->offsets_.push_back(typeOffsets[idx]);
    }
    z->initialOffset_ = typeOffsets[0];
    at += blockSize(c, timeSize);
    if (version >= '2' && at < b.size() && b[at] == '\n') {
        size_t end = b.find('\n', at + 1);
        if (end != std::string_view::npos && end > at + 1) {
            std::string footer(b.substr(at + 1, end - at - 1));
            z->rule_ = parseRule(footer);
            if (!z->rule_) return invalid("has a rule Fourfold can't read: " + footer);
        }
    }
    return std::shared_ptr<const TimeZone>(std::move(z));
}

Result<std::shared_ptr<const TimeZone>> TimeZone::load(const std::string& name) {
    if (name == "UTC" || name == "Etc/UTC") return utc();
    if (!isTimeZoneName(name)) return makeError(ErrorCode::Invalid, "\"" + name + "\" isn't a time zone name");
    std::string dir = "/usr/share/zoneinfo";
    if (const char* env = std::getenv("TZDIR"); env && *env) dir = env;
    std::ifstream in(dir + "/" + name, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::NotFound, "there's no time zone named \"" + name + "\" (in " + dir +
                                                  "); use a name like \"America/Denver\" or \"Europe/Paris\"");
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > 1024 * 1024) return makeError(ErrorCode::Invalid, "the zoneinfo file for " + name + " is too big");
    return fromTzif(name, bytes);
}

int32_t TimeZone::offsetAt(int64_t utc) const {
    if (transitions_.empty()) return rule_ ? rule_->offsetAt(utc) : initialOffset_;
    if (utc < transitions_.front()) return initialOffset_;
    if (utc >= transitions_.back() && rule_) return rule_->offsetAt(utc);
    auto it = std::upper_bound(transitions_.begin(), transitions_.end(), utc);
    return offsets_[static_cast<size_t>(it - transitions_.begin()) - 1];
}

int64_t TimeZone::toUtc(int64_t local, LocalKind* kind) const {
    std::set<int32_t> offsets{offsetAt(local - kDay), offsetAt(local), offsetAt(local + kDay)};
    std::vector<int64_t> valid;
    for (int32_t o : offsets) {
        int64_t u = local - o;
        if (offsetAt(u) == o) valid.push_back(u);
    }
    std::sort(valid.begin(), valid.end());
    valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
    if (!valid.empty()) {
        if (kind) *kind = valid.size() > 1 ? LocalKind::Repeated : LocalKind::Unique;
        return valid.front();
    }
    // Skipped: the instant of the jump, between the readings with the
    // offsets before and after it.
    if (kind) *kind = LocalKind::Skipped;
    int64_t a = local - offsetAt(local - kDay), b = local - offsetAt(local + kDay);
    if (a > b) std::swap(a, b);
    int32_t before = offsetAt(a);
    while (b - a > 1) {
        int64_t mid = a + (b - a) / 2;
        (offsetAt(mid) == before ? a : b) = mid;
    }
    return b;
}

}  // namespace fourfold

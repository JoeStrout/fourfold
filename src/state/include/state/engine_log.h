// EngineLog: the engine's own log, in private/state/logs/ (overview §1):
// requests, script errors, refused requests, and failures. App code can't
// reach it, so it can't forge or bury entries there; the app's own output
// goes to the dropbox, a separate type.
//
// One file per day (UTC), "logs/engine-2026-10-25.log", one line per entry:
//   2026-10-25T20:31:02.123Z csrf POST /guestbook/sign: no CSRF cookie
// Control characters in a message are escaped, so a message can't make a line
// of its own. Rotation and retention come in M11.
//
// write() is thread-safe and never waits for the disk: lines go to a
// background thread.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "base/clock.h"
#include "partition/data_dir.h"

namespace fourfold {

class EngineLog {
public:
    // The clock must outlive the log.
    EngineLog(StateDir state, const Clock& clock = systemClock());
    // Writes what's queued, then stops.
    ~EngineLog();

    EngineLog(const EngineLog&) = delete;
    EngineLog& operator=(const EngineLog&) = delete;

    // `category` is one word ("request", "error", "csrf", "session", "start").
    void write(const std::string& category, const std::string& message);

    // Waits until everything written so far is on disk. For tests.
    void flush();

    // The current file's path within private/state/, e.g.
    // "logs/engine-2026-10-25.log".
    std::string currentFile() const;

private:
    void writerMain();

    StateDir state_;
    const Clock& clock_;
    std::mutex mutex_;
    std::condition_variable wake_, drained_;
    std::deque<std::pair<int64_t, std::string>> queue_;   // (time, line without the time)
    bool writing_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

// The engine's secret key (32 random bytes, in private/state/secret.key):
// made on first use, and the same afterwards, across restarts and in every
// process of the app. CSRF tokens are keyed with it.
Result<std::string> loadOrCreateEngineKey(const StateDir& state);

}  // namespace fourfold

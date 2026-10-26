#include "state/engine_log.h"

#include <cstdio>
#include <ctime>

#include "platform/random.h"

namespace fourfold {

namespace {

constexpr size_t kKeyBytes = 32;

std::tm utc(int64_t millis) {
    std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm t{};
    gmtime_r(&seconds, &t);
    return t;
}

std::string fileFor(int64_t millis) {
    std::tm t = utc(millis);
    char buf[64];
    std::snprintf(buf, sizeof buf, "logs/engine-%04d-%02d-%02d.log", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buf;
}

std::string timestamp(int64_t millis) {
    std::tm t = utc(millis);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
                  t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, static_cast<int>(millis % 1000));
    return buf;
}

// Control characters as \n, \r, \t or \xNN, and '\' as "\\".
std::string escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

}  // namespace

EngineLog::EngineLog(StateDir state, const Clock& clock) : state_(std::move(state)), clock_(clock) {
    thread_ = std::thread([this] { writerMain(); });
}

EngineLog::~EngineLog() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void EngineLog::write(const std::string& category, const std::string& message) {
    int64_t now = clock_.nowMillis();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.emplace_back(now, escape(category) + " " + escape(message));
    }
    wake_.notify_one();
}

void EngineLog::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    drained_.wait(lock, [this] { return queue_.empty() && !writing_; });
}

std::string EngineLog::currentFile() const { return fileFor(clock_.nowMillis()); }

void EngineLog::writerMain() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_) return;
        auto batch = std::move(queue_);
        queue_.clear();
        writing_ = true;
        lock.unlock();

        // One append per file per batch.
        std::string file, text;
        auto put = [&] {
            if (text.empty()) return;
            Status s = state_.append(RelPath::parse(file).value(), text);
            if (!s) std::fprintf(stderr, "fourfold: can't write the engine log: %s\n", s.error().toString().c_str());
            text.clear();
        };
        for (const auto& [time, line] : batch) {
            std::string f = fileFor(time);
            if (f != file) {
                put();
                file = f;
            }
            text += timestamp(time) + " " + line + "\n";
        }
        put();

        lock.lock();
        writing_ = false;
        drained_.notify_all();
    }
}

Result<std::string> loadOrCreateEngineKey(const StateDir& state) {
    RelPath path = RelPath::parse("secret.key").value();
    auto existing = state.read(path, 1024);
    if (existing) {
        if (existing.value().size() != kKeyBytes) {
            return makeError(ErrorCode::Invalid, "private/state/secret.key: damaged (expected " +
                                                     std::to_string(kKeyBytes) + " bytes)");
        }
        return existing;
    }
    if (existing.error().code != ErrorCode::NotFound) return existing.error();
    // Two processes starting at once may both get here; one creates the
    // file, and both then read that one.
    Status s = state.create(path, randomBytes(kKeyBytes));
    if (!s && s.error().code != ErrorCode::Conflict) return s.error();
    return state.read(path, 1024);
}

}  // namespace fourfold

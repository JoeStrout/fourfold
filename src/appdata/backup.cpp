// Backups. The partitions are copied through DataDir and StateDir (one
// segment at a time, never following links); the backup folder itself is
// the engine's own, reached by host path.

#include "appdata/backup.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "base/version.h"
#include "platform/sqlite.h"

namespace fourfold {

namespace fs = std::filesystem;

namespace {

const char* const kManifest = "backup.json";

bool isSqliteFile(const std::string& path, const std::string& base) {
    return path == base || path == base + "-wal" || path == base + "-shm" || path == base + "-journal";
}

// What a backup leaves out of data/: lock files, and the database, which
// is copied through SQLite.
bool skipDataOut(const std::string& path, bool) {
    return path == ".fourfold/locks" || isSqliteFile(path, ".fourfold/app.sqlite");
}

bool skipDataIn(const std::string& path, bool) { return path == ".fourfold/locks"; }

bool skipDataInButDatabase(const std::string& path, bool folder) {
    return skipDataIn(path, folder) || isSqliteFile(path, ".fourfold/app.sqlite");
}

// And of private/state/: the backups themselves, locks, logs, requests to
// the server, CGI mode's compiled versions (copies of code/, made again as
// needed), and state.db (through SQLite).
bool skipStateIn(const std::string& path, bool) {
    return path == "backups" || path == "locks" || path == "logs" || path == "control" || path == "cgi";
}

bool skipStateOut(const std::string& path, bool folder) {
    return skipStateIn(path, folder) || isSqliteFile(path, "state.db");
}

bool exists(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0;
}

std::string timeText(int64_t millis, const char* format) {
    std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[40];
    std::strftime(buffer, sizeof buffer, format, &utc);
    return buffer;
}

// Copies a live SQLite database into `to`, consistently.
Status copySqlite(const std::string& from, const std::string& to) {
    auto db = Sqlite::open(from);
    if (!db) return db.error();
    return db.value()->backupTo(to);
}

Result<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return makeError(ErrorCode::NotFound, path + ": can't read");
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool isAutomatic(const std::string& reason) { return reason == "before-migrations" || reason == "scheduled"; }

}  // namespace

BackupService::BackupService(const DataDir& data, const StateDir& state, std::string folder, bool sqliteDatabase,
                             const Clock& clock)
    : data_(data), state_(state), folder_(std::move(folder)), sqlite_(sqliteDatabase), clock_(clock) {}

Result<BackupInfo> BackupService::backup(const std::string& reason, const std::vector<int>& migrations) {
    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (ec) return makeError(ErrorCode::IO, folder_ + ": " + ec.message());
    fs::permissions(folder_, fs::perms::owner_all, fs::perm_options::replace, ec);

    BackupInfo info;
    int64_t now = clock_.nowMillis();
    info.time = timeText(now, "%Y-%m-%dT%H:%M:%SZ");
    info.reason = reason;
    std::string base = timeText(now, "%Y%m%d-%H%M%S") + "-" + reason;
    info.name = base;
    for (int n = 2; exists(folder_ + "/" + info.name); n++) info.name = base + "-" + std::to_string(n);
    info.path = folder_ + "/" + info.name;
    // Written under a temporary name, renamed once complete: a backup
    // that's listed is whole.
    std::string partial = folder_ + "/.partial-" + info.name;
    fs::remove_all(partial, ec);
    fs::create_directories(partial, ec);
    if (ec) return makeError(ErrorCode::IO, partial + ": " + ec.message());

    auto fail = [&](const Error& e) -> Result<BackupInfo> {
        std::error_code ignored;
        fs::remove_all(partial, ignored);
        return makeError(e.code, "backup: " + e.message);
    };
    if (Status s = data_.copyOut(partial + "/data", skipDataOut); !s) return fail(s.error());
    if (sqlite_) {
        auto live = data_.databasePath();
        if (!live) return fail(live.error());
        if (exists(live.value())) {
            fs::create_directories(partial + "/data/.fourfold", ec);
            if (Status s = copySqlite(live.value(), partial + "/data/.fourfold/app.sqlite"); !s) return fail(s.error());
        }
    }
    if (Status s = state_.copyOut(partial + "/state", skipStateOut); !s) return fail(s.error());
    auto stateDb = state_.sqlitePath(RelPath::parse("state.db").value());
    if (!stateDb) return fail(stateDb.error());
    if (exists(stateDb.value())) {
        if (Status s = copySqlite(stateDb.value(), partial + "/state/state.db"); !s) return fail(s.error());
    }

    Datum::ListItems numbers;
    for (int m : migrations) numbers.push_back(Datum::number(m));
    Datum manifest = Datum::map({
        {"time", Datum::string(info.time)},
        {"reason", Datum::string(reason)},
        {"fourfold", Datum::string(versionString())},
        {"database", Datum::string(sqlite_ ? "sqlite (included)" : "none, or Postgres (not included; use pg_dump)")},
        {"migrations", Datum::list(std::move(numbers))},
        {"state", Datum::number(1)},
    });
    {
        std::ofstream out(partial + "/" + kManifest, std::ios::binary);
        out << toJson(manifest) << "\n";
        if (!out) return fail(makeError(ErrorCode::IO, partial + "/" + kManifest + ": can't write"));
    }
    fs::rename(partial, info.path, ec);
    if (ec) return fail(makeError(ErrorCode::IO, info.path + ": " + ec.message()));
    return info;
}

Status BackupService::restore(const std::string& name, bool withState, bool withDatabase) {
    if (name.empty() || name.find('/') != std::string::npos || name[0] == '.') {
        return makeError(ErrorCode::Invalid, "restore: '" + name + "' isn't a backup's name");
    }
    std::string path = folder_ + "/" + name;
    if (!exists(path + "/" + kManifest)) {
        return makeError(ErrorCode::NotFound, "restore: there's no backup named " + name + " in " + folder_);
    }
    // data/: everything, including the SQLite database, which is a
    // consistent copy (no WAL to go with it), and which no one has open.
    if (Status s = data_.copyIn(path + "/data", withDatabase ? skipDataIn : skipDataInButDatabase); !s) return s;
    if (sqlite_) (void)data_.databasePath();   // data/.fourfold/, for the database, even if the backup had none
    if (withState) {
        if (!exists(path + "/state")) return makeError(ErrorCode::NotFound, "restore: " + name + " has no state");
        if (Status s = state_.copyIn(path + "/state", skipStateIn); !s) return s;
    }
    return {};
}

Result<std::vector<BackupInfo>> BackupService::list() const {
    std::vector<BackupInfo> out;
    std::error_code ec;
    if (!exists(folder_)) return out;
    for (const fs::directory_entry& e : fs::directory_iterator(folder_, ec)) {
        std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        auto manifest = readFile(e.path().string() + "/" + kManifest);
        if (!manifest) continue;
        auto parsed = parseJson(manifest.value());
        if (!parsed || parsed.value().kind() != Datum::Kind::Map) continue;
        const Datum::MapItems& m = parsed.value().asMap();
        BackupInfo info;
        info.name = name;
        info.path = e.path().string();
        if (auto it = m.find("time"); it != m.end() && it->second.kind() == Datum::Kind::String) {
            info.time = it->second.asString();
        }
        if (auto it = m.find("reason"); it != m.end() && it->second.kind() == Datum::Kind::String) {
            info.reason = it->second.asString();
        }
        info.hasState = exists(info.path + "/state");
        out.push_back(std::move(info));
    }
    if (ec) return makeError(ErrorCode::IO, folder_ + ": " + ec.message());
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) {
        return a.time != b.time ? a.time > b.time : a.name > b.name;
    });
    return out;
}

Status BackupService::prune(size_t keep) {
    auto all = list();
    if (!all) return all.error();
    size_t kept = 0;
    for (const BackupInfo& b : all.value()) {
        if (!isAutomatic(b.reason)) continue;
        if (kept++ < keep) continue;
        std::error_code ec;
        fs::remove_all(b.path, ec);
        if (ec) return makeError(ErrorCode::IO, b.path + ": " + ec.message());
    }
    return {};
}

}  // namespace fourfold

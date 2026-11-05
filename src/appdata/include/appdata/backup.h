// BackupService: snapshots of an app's data and state (overview §3,
// Backups; Arch §5 appdata).
//
// A backup is a folder, by default in private/state/backups/, or in a
// configured folder outside every partition (it holds the auth store, so it
// must never be where app code or the web can reach):
//
//   20261105-140212-before-migrations/
//     backup.json      when, why, the database, the migrations applied
//     data/            every file in data/, and the SQLite database
//     state/           private/state/: sessions, users, the engine key
//
// SQLite databases are copied with SQLite's online backup API, never as
// files, so a write in progress can't spoil the copy. Other files are
// copied as they are: the caller makes sure nothing writes them meanwhile
// (the app is paused, as for migrations, or stopped). A Postgres database
// isn't in the backup; it's backed up with its own tools (pg_dump).
//
// Blocking: the lifecycle thread, a worker, or the CLI.
#pragma once

#include <string>
#include <vector>

#include "base/clock.h"
#include "base/datum.h"
#include "base/result.h"
#include "partition/data_dir.h"

namespace fourfold {

struct BackupInfo {
    std::string name;     // "20261105-140212-before-migrations"
    std::string path;     // its folder's host path
    std::string reason;   // "manual", "before-migrations", "scheduled", "before-restore"
    std::string time;     // "2026-11-05T14:02:12Z"
    bool hasState = true; // private/state/ is in it
};

class BackupService {
public:
    // `folder`: where backups go (a host path, created if missing). `data`
    // and `state` must outlive the service. `sqliteDatabase`: whether the
    // app's database is SQLite, in data/ (else Postgres, or none).
    BackupService(const DataDir& data, const StateDir& state, std::string folder, bool sqliteDatabase,
                  const Clock& clock = systemClock());

    // Takes a backup. `migrations`: the numbers applied, recorded in
    // backup.json for whoever restores it.
    Result<BackupInfo> backup(const std::string& reason, const std::vector<int>& migrations = {});

    // Puts a backup back: data/ (with the SQLite database, unless not
    // `withDatabase`) and, if `withState`, private/state/ too. What's there
    // now is replaced. The app must be paused or stopped, and, if the
    // database is restored, no connection to it open (Database::closeIdle).
    Status restore(const std::string& name, bool withState, bool withDatabase = true);

    // Newest first.
    Result<std::vector<BackupInfo>> list() const;

    // Removes the oldest automatic backups ("before-migrations" and
    // "scheduled") beyond the newest `keep` of them. Others stay.
    Status prune(size_t keep);

    const std::string& folder() const { return folder_; }

private:
    const DataDir& data_;
    const StateDir& state_;
    std::string folder_;
    bool sqlite_;
    const Clock& clock_;
};

}  // namespace fourfold

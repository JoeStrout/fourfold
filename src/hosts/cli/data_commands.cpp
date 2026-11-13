// `fourfold migrate`, `fourfold backup` and `fourfold restore` (overview §3,
// Applying migrations; Backups).
//
// migrate and backup ask the app's server to do it, if one is running
// (through a request file in private/state/control/, which the server
// checks every quarter second), and wait for its answer; otherwise they
// open the app themselves, holding its server lock so no server starts
// meanwhile. restore needs the app stopped.

#include "data_commands.h"

#include <chrono>
#include <optional>
#include <ostream>
#include <sstream>
#include <thread>

#include "app_server.h"
#include "base/crypto.h"
#include "cgi/schema_lock.h"
#include "config/app_config.h"
#include "partition/app_folder.h"
#include "platform/random.h"

namespace fourfold::cli {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

struct DataArgs {
    std::string folder;
    std::string backup;           // restore: the backup's name
    std::string serverConfig;
    std::string dropboxSocket;    // the dropbox writer's, if it owns dropbox/
    bool allowOlderCode = false;
    bool dataOnly = false;        // restore: not private/state/
    bool list = false;            // restore: list the backups
};

bool parseArgs(const std::string& command, const std::vector<std::string>& args, DataArgs& out, std::string& error) {
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--server-config" || a == "--dropbox-socket") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                error = a + (a == "--server-config" ? " needs a file" : " needs a socket path");
                return false;
            }
            (a == "--server-config" ? out.serverConfig : out.dropboxSocket) = args[++i];
        } else if (a == "--allow-older-code" && command == "migrate") {
            out.allowOlderCode = true;
        } else if (a == "--data-only" && command == "restore") {
            out.dataOnly = true;
        } else if (a == "--list" && command == "restore") {
            out.list = true;
        } else if (!a.empty() && a[0] == '-') {
            error = "unknown option '" + a + "'";
            return false;
        } else if (out.folder.empty()) {
            out.folder = a;
        } else if (command == "restore" && out.backup.empty()) {
            out.backup = a;
        } else {
            error = "too many arguments";
            return false;
        }
    }
    if (out.folder.empty()) {
        error = "no app folder given";
        return false;
    }
    if (command == "restore" && out.backup.empty()) out.list = true;
    return true;
}

// Asks the running server to carry out `command` ("migrate", "backup"),
// and prints its answer.
int askServer(const std::string& folder, const std::string& command, std::ostream& out, std::ostream& err) {
    auto app = AppFolder::open(folder, AppFolder::Missing::Fail);
    auto state = app ? app.value().stateDir() : Result<StateDir>(app.error());
    if (!state) {
        err << "fourfold " << command << ": " << state.error().message << "\n";
        return kExitError;
    }
    RelPath request = RelPath::parse("control/" + command + ".request").value();
    // Another request of the same kind, not yet picked up: wait for it.
    for (int i = 0; i < 400; i++) {
        auto pending = state.value().exists(request);
        if (!pending || !pending.value()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::string id = toHex(randomBytes(8));
    if (Status s = state.value().write(request, id + "\n"); !s) {
        err << "fourfold " << command << ": " << s.error().message << "\n";
        return kExitError;
    }
    out << "The app's server is running; asked it to " << (command == "migrate" ? "apply the migrations" : "take a backup")
        << ". Waiting..." << std::endl;
    RelPath answer = RelPath::parse("control/" + command + "-" + id + ".result").value();
    auto started = std::chrono::steady_clock::now();
    bool pickedUp = false;
    while (true) {
        auto done = state.value().exists(answer);
        if (done && done.value()) break;
        if (!pickedUp) {
            auto waiting = state.value().exists(request);
            pickedUp = waiting && !waiting.value();
            if (!pickedUp && std::chrono::steady_clock::now() - started > std::chrono::seconds(10)) {
                (void)state.value().remove(request);
                err << "fourfold " << command << ": the server didn't pick up the request; is it still running?\n";
                return kExitError;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    auto text = state.value().read(answer, 64 * 1024);
    (void)state.value().remove(answer);
    auto parsed = text ? parseJson(text.value()) : Result<Datum>(text.error());
    if (!parsed || parsed.value().kind() != Datum::Kind::Map) {
        err << "fourfold " << command << ": couldn't read the server's answer\n";
        return kExitError;
    }
    const Datum::MapItems& m = parsed.value().asMap();
    bool ok = m.count("ok") && m.at("ok") == Datum::number(1);
    std::string message = m.count("message") && m.at("message").kind() == Datum::Kind::String
                              ? m.at("message").asString()
                              : "";
    (ok ? out : err) << message << "\n";
    return ok ? kExitOk : kExitError;
}

// The schema lock that CGI requests to the app hold (overview §8): taken
// exclusively, so a migrate or restore waits for those running, and those
// arriving wait (or get a 503). Held as long as `held` lives.
struct CgiHold {
    std::optional<StateDir> state;
    std::optional<SchemaFileLock> lock;
    std::optional<SchemaFileLock::Exclusive> held;
};

bool holdCgiRequests(const std::string& folder, CgiHold& hold, std::ostream& err, const char* command) {
    auto app = AppFolder::open(folder);
    auto state = app ? app.value().stateDir() : Result<StateDir>(app.error());
    if (!state) {
        err << command << state.error().message << "\n";
        return false;
    }
    hold.state = std::move(state).value();
    hold.lock.emplace(*hold.state);
    auto held = hold.lock->exclusive();
    if (!held) {
        err << command << held.error().message << "\n";
        return false;
    }
    hold.held = std::move(held).value();
    return true;
}

ServerOptions offlineOptions(const DataArgs& args) {
    ServerOptions options;
    options.dev = false;
    options.listen = false;
    options.serverConfig = args.serverConfig;
    options.dropboxSocket = args.dropboxSocket;
    options.allowOlderCode = args.allowOlderCode;
    return options;
}

}  // namespace

int runMigrate(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    DataArgs a;
    std::string problem;
    if (!parseArgs("migrate", args, a, problem)) {
        err << "fourfold migrate: " << problem
            << "\nUsage: fourfold migrate <app-folder> [--server-config FILE] [--dropbox-socket PATH] [--allow-older-code]\n";
        return kExitUsage;
    }
    if (serverRunning(a.folder)) return askServer(a.folder, "migrate", out, err);
    auto server = AppServer::start(a.folder, offlineOptions(a), out, err);
    if (!server) return kExitError;
    CgiHold hold;
    if (!holdCgiRequests(a.folder, hold, err, "fourfold migrate: ")) return kExitError;
    if (!server->compiled() && server->instance().pendingMigrations() == 0) {
        err << "fourfold migrate: the app doesn't compile, or can't run on this data (above); nothing was changed\n";
        return kExitError;
    }
    MigrateResult r = server->instance().migrateNow();
    (r.ok ? out : err) << r.message << "\n";
    return r.ok ? kExitOk : kExitError;
}

int runBackup(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    DataArgs a;
    std::string problem;
    if (!parseArgs("backup", args, a, problem)) {
        err << "fourfold backup: " << problem << "\nUsage: fourfold backup <app-folder> [--server-config FILE] [--dropbox-socket PATH]\n";
        return kExitUsage;
    }
    if (serverRunning(a.folder)) return askServer(a.folder, "backup", out, err);
    std::ostringstream quiet;   // compile output doesn't matter here
    auto server = AppServer::start(a.folder, offlineOptions(a), quiet, err);
    if (!server) return kExitError;
    auto b = server->instance().backupNow("manual");
    if (!b) {
        err << "fourfold backup: " << b.error().message << "\n";
        return kExitError;
    }
    out << "Backup " << b.value().name << " is in " << b.value().path << "\n";
    return kExitOk;
}

int runRestore(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    DataArgs a;
    std::string problem;
    if (!parseArgs("restore", args, a, problem)) {
        err << "fourfold restore: " << problem
            << "\nUsage: fourfold restore <app-folder> <backup-name> [--data-only] [--server-config FILE]\n"
               "       fourfold restore <app-folder> --list\n";
        return kExitUsage;
    }
    if (serverRunning(a.folder)) {
        err << "fourfold restore: the app is running; stop its server first (a restore replaces its data)\n";
        return kExitError;
    }
    std::ostringstream quiet;
    auto server = AppServer::start(a.folder, offlineOptions(a), quiet, err);
    if (!server) return kExitError;
    BackupService& backups = server->backups();
    if (a.list) {
        auto all = backups.list();
        if (!all) {
            err << "fourfold restore: " << all.error().message << "\n";
            return kExitError;
        }
        if (all.value().empty()) out << "No backups in " << backups.folder() << "\n";
        for (const BackupInfo& b : all.value()) out << b.name << "  (" << b.reason << ", " << b.time << ")\n";
        return kExitOk;
    }
    CgiHold hold;
    if (!holdCgiRequests(a.folder, hold, err, "fourfold restore: ")) return kExitError;
    // A backup of what's there now, first: a restore can be undone too.
    auto before = backups.backup("before-restore");
    if (!before) {
        err << "fourfold restore: couldn't back up the current data first, so nothing was changed: "
            << before.error().message << "\n";
        return kExitError;
    }
    if (Database* db = server->database()) db->closeIdle();
    if (Status s = backups.restore(a.backup, !a.dataOnly, true); !s) {
        err << "fourfold restore: " << s.error().message << "\nThe data from before is in backup "
            << before.value().name << ".\n";
        return kExitError;
    }
    out << "Restored " << a.backup << (a.dataOnly ? " (data only)" : "") << ". What was there before is in backup "
        << before.value().name << ".\n";
    return kExitOk;
}

}  // namespace fourfold::cli

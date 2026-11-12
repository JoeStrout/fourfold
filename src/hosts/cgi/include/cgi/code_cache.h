// CodeCache: CGI mode's compiled app, kept between requests (overview §8,
// Hosting modes; M10).
//
// A CGI process lives for one request, so whatever the standalone server
// keeps in memory has to come from private/state/ here. What's kept is a
// *version*: a snapshot of code/ (private/state/cgi/v<N>/code/), and what a
// full compile of it found that's costly to find again, which is every .ms
// file's handlers (running each file's top level is most of a compile), and
// which .ms file each route's code is in. A request compiles the snapshot
// with those handlers known, so of the MiniScript it compiles only the
// requested page's file and the libs that file imports (CompileOptions::
// knownHandlers). Templates and the rest are compiled again, which is
// quick.
//
// Serving from a snapshot keeps overview §8's promise in CGI mode too: a
// broken or partial upload never takes the site down, because each request
// sees the code exactly as its version compiled it, and a version that
// doesn't compile is never installed.
//
// When code/ changes, a request notices (it lists code/ each time, and
// compares a fingerprint), and records the change as pending. A request that
// finds the same change pending for the quiet period compiles it: the tree
// has stopped changing, as far as requests can tell, which works whatever
// times the uploader gave the files. Touching code/.deploy compiles at once.
// One process compiles at a time; the others keep serving the current
// version meanwhile, except when there's none yet, when they wait for it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/result.h"
#include "compiledapp/compiled_app.h"
#include "compiledapp/sql.h"
#include "partition/code_tree.h"
#include "partition/data_dir.h"
#include "script/script_host.h"

namespace fourfold {

struct CodeCacheOptions {
    // How long a change to code/ must stay the same before it's compiled.
    int64_t quietMillis = 2000;
    // How long a replaced version's snapshot is kept, for requests that
    // started on it.
    int64_t retireMillis = 10 * 60 * 1000;
    // The app's database (from its config): its queries and migrations are
    // the ones compiled. Changing it compiles a new version.
    SqlDialect dialect = SqlDialect::Sqlite;
    const Clock* clock = &systemClock();
    // Called with a new version once it has compiled, before it's installed
    // (CGI mode applies its migrations here). An error refuses the version,
    // as a failed compile does.
    std::function<Status(const std::shared_ptr<const CompiledApp>&)> beforeInstall;
};

struct CodeLoad {
    // The version this request is served with: complete but for its
    // scripts, which hold only what the request needs (all of them for a
    // full load, or just after compiling a new version).
    std::shared_ptr<const CompiledApp> app;
    // The snapshot it was compiled from: the engine's code/.
    std::optional<CodeTree> code;
    uint64_t generation = 0;
    // This request compiled a new version and installed it.
    bool compiled = false;
    // This request compiled a new version that failed, with its diagnostics
    // (or its migrations failed); the previous version still serves.
    std::vector<Diagnostic> failed;
};

class CodeCache {
public:
    // Everything must outlive the cache.
    CodeCache(const StateDir& state, const CodeTree& code, ScriptHost& scripts, CodeCacheOptions options = {});

    // The version to serve request target `target` ("/guestbook?page=2")
    // with, compiling a new one first if code/ has changed and gone quiet.
    // Fails only if there's no version to serve: the app has never
    // compiled (its errors are in the error's message).
    Result<CodeLoad> load(const std::string& target);

    // The same, with all of the version's scripts compiled (for `fourfold
    // run-jobs`, which may run any job).
    Result<CodeLoad> loadFull();

private:
    struct State;
    struct Version;

    Result<CodeLoad> load(const std::string* target);
    Result<State> readState() const;
    Status writeState(const State& state) const;
    // Compiles and installs a new version from `listing`; `state` is
    // updated. Holds the compile lock.
    Result<CodeLoad> compileNew(const CodeListing& listing, const std::string& key, State& state);
    // Serves version `generation`, compiling only what `target` needs
    // (everything, if null).
    Result<CodeLoad> serve(uint64_t generation, const std::string* target);
    void removeRetired(State& state) const;

    const StateDir& state_;
    const CodeTree& code_;
    ScriptHost& scripts_;
    CodeCacheOptions options_;
};

// The key a compiled version is known by: a fingerprint of the listing
// (each file's path, size and time), the deploy trigger's time, and the
// database dialect. Exposed for tests.
std::string codeFingerprint(const CodeListing& listing, std::optional<int64_t> deployTrigger, SqlDialect dialect);

// PageHandlers as JSON-able data, and back (for version.json).
Datum handlersToDatum(const PageHandlers& handlers);
std::optional<PageHandlers> handlersFromDatum(const Datum& datum);

}  // namespace fourfold

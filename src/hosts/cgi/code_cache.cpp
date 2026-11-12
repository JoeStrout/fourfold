#include "cgi/code_cache.h"

#include <algorithm>

#include "base/crypto.h"
#include "base/datum.h"
#include "compiler/compiler.h"
#include "partition/file_lock.h"

namespace fourfold {

namespace {

const RelPath& stateFile() {
    static const RelPath p = RelPath::parse("cgi/state.json").value();
    return p;
}

RelPath versionFolder(uint64_t generation) { return RelPath::parse("cgi/v" + std::to_string(generation)).value(); }
RelPath versionFile(uint64_t generation) { return versionFolder(generation).child("version.json").value(); }
RelPath snapshotFolder(uint64_t generation) { return versionFolder(generation).child("code").value(); }

constexpr size_t kMaxStateBytes = 1024 * 1024;
constexpr size_t kMaxVersionBytes = 64 * 1024 * 1024;

Datum strings(const std::vector<std::string>& items) {
    Datum::ListItems list;
    for (const std::string& s : items) list.push_back(Datum::string(s));
    return Datum::list(std::move(list));
}

const Datum* field(const Datum& map, const std::string& name) {
    if (map.kind() != Datum::Kind::Map) return nullptr;
    auto it = map.asMap().find(name);
    return it == map.asMap().end() ? nullptr : &it->second;
}

std::optional<std::string> stringField(const Datum& map, const std::string& name) {
    const Datum* d = field(map, name);
    if (!d || d->kind() != Datum::Kind::String) return std::nullopt;
    return d->asString();
}

std::optional<double> numberField(const Datum& map, const std::string& name) {
    const Datum* d = field(map, name);
    if (!d || d->kind() != Datum::Kind::Number) return std::nullopt;
    return d->asNumber();
}

bool stringsFrom(const Datum* d, std::vector<std::string>& out) {
    if (!d) return true;   // absent: none
    if (d->kind() != Datum::Kind::List) return false;
    for (const Datum& item : d->asList()) {
        if (item.kind() != Datum::Kind::String) return false;
        out.push_back(item.asString());
    }
    return true;
}

// The .ms file whose code serves each route (by URL path, Markdown URLs
// included), for routes that run code.
std::map<std::string, std::string> codeFiles(const CompiledApp& app) {
    std::map<std::string, std::string> files;
    for (const auto& [path, entry] : app.routes) {
        for (const auto* h : {&entry.render, &entry.get, &entry.post, &entry.webhook}) {
            if (*h) {
                files[path] = (*h)->source.str();
                break;
            }
        }
    }
    for (const auto& [alias, path] : app.routes.markdownAliases()) {
        auto it = files.find(path);
        if (it != files.end()) files[alias] = it->second;
    }
    return files;
}

}  // namespace

// --- Handlers as data ---

Datum handlersToDatum(const PageHandlers& h) {
    Datum::MapItems m;
    if (h.render) m["render"] = Datum::number(1);
    if (!h.get.empty()) m["get"] = strings(h.get);
    if (!h.post.empty()) m["post"] = strings(h.post);
    if (!h.webhook.empty()) m["webhook"] = strings(h.webhook);
    if (!h.job.empty()) m["job"] = strings(h.job);
    if (h.usesSession) m["session"] = Datum::number(1);
    if (h.access) m["access"] = *h.access;
    if (!h.forms.empty()) {
        Datum::MapItems forms(h.forms.begin(), h.forms.end());
        m["forms"] = Datum::map(std::move(forms));
    }
    return Datum::map(std::move(m));
}

std::optional<PageHandlers> handlersFromDatum(const Datum& d) {
    if (d.kind() != Datum::Kind::Map) return std::nullopt;
    PageHandlers h;
    h.render = field(d, "render") != nullptr;
    h.usesSession = field(d, "session") != nullptr;
    if (!stringsFrom(field(d, "get"), h.get) || !stringsFrom(field(d, "post"), h.post) ||
        !stringsFrom(field(d, "webhook"), h.webhook) || !stringsFrom(field(d, "job"), h.job)) {
        return std::nullopt;
    }
    if (const Datum* access = field(d, "access")) h.access = *access;
    if (const Datum* forms = field(d, "forms")) {
        if (forms->kind() != Datum::Kind::Map) return std::nullopt;
        for (const auto& [name, spec] : forms->asMap()) h.forms.emplace(name, spec);
    }
    return h;
}

std::string codeFingerprint(const CodeListing& listing, std::optional<int64_t> deployTrigger, SqlDialect dialect) {
    std::string s = dialect == SqlDialect::Postgres ? "pg\n" : "sqlite\n";
    s += "deploy " + (deployTrigger ? std::to_string(*deployTrigger) : std::string("-")) + "\n";
    for (const CodeEntry& e : listing.files) {
        s += e.path.str();
        s += '\0';
        s += std::to_string(e.size) + " " + std::to_string(e.modifiedNs) + "\n";
    }
    for (const std::string& skipped : listing.skipped) s += "skipped " + skipped + "\n";
    return toHex(hashBytes(s)).substr(0, 32);
}

// --- State ---

// private/state/cgi/state.json: which version serves, and what's pending.
struct CodeCache::State {
    uint64_t current = 0;            // 0: none yet
    uint64_t lastGeneration = 0;     // the newest made, installed or not
    std::string key;                 // the current version's codeFingerprint
    int64_t deploy = -1;             // its deploy trigger's time (-1: none)
    std::string pending;             // a change seen, waiting for quiet
    int64_t pendingSince = 0;        // when it was first seen (ms)
    std::string failed;              // the last key that failed to compile
    std::vector<std::pair<uint64_t, int64_t>> retired;   // replaced versions, and when (ms)
};

Result<CodeCache::State> CodeCache::readState() const {
    State st;
    auto text = state_.read(stateFile(), kMaxStateBytes);
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) return st;
        return text.error();
    }
    auto d = parseJson(text.value());
    if (!d || d.value().kind() != Datum::Kind::Map) return st;   // unreadable: start again
    const Datum& m = d.value();
    st.current = static_cast<uint64_t>(numberField(m, "current").value_or(0));
    st.lastGeneration = static_cast<uint64_t>(numberField(m, "lastGeneration").value_or(st.current));
    st.key = stringField(m, "key").value_or("");
    st.deploy = static_cast<int64_t>(numberField(m, "deploy").value_or(-1));
    st.pending = stringField(m, "pending").value_or("");
    st.pendingSince = static_cast<int64_t>(numberField(m, "pendingSince").value_or(0));
    st.failed = stringField(m, "failed").value_or("");
    if (const Datum* retired = field(m, "retired"); retired && retired->kind() == Datum::Kind::List) {
        for (const Datum& r : retired->asList()) {
            if (r.kind() != Datum::Kind::List || r.asList().size() != 2) continue;
            const Datum& g = r.asList()[0];
            const Datum& t = r.asList()[1];
            if (g.kind() != Datum::Kind::Number || t.kind() != Datum::Kind::Number) continue;
            st.retired.emplace_back(static_cast<uint64_t>(g.asNumber()), static_cast<int64_t>(t.asNumber()));
        }
    }
    return st;
}

Status CodeCache::writeState(const State& st) const {
    Datum::ListItems retired;
    for (const auto& [g, t] : st.retired) {
        retired.push_back(Datum::list({Datum::number(static_cast<double>(g)), Datum::number(static_cast<double>(t))}));
    }
    // Times in ms and ns fit a double exactly (to 2^53) for centuries.
    Datum d = Datum::map({{"current", Datum::number(static_cast<double>(st.current))},
                          {"lastGeneration", Datum::number(static_cast<double>(st.lastGeneration))},
                          {"key", Datum::string(st.key)},
                          {"deploy", Datum::number(static_cast<double>(st.deploy))},
                          {"pending", Datum::string(st.pending)},
                          {"pendingSince", Datum::number(static_cast<double>(st.pendingSince))},
                          {"failed", Datum::string(st.failed)},
                          {"retired", Datum::list(std::move(retired))}});
    return state_.write(stateFile(), toJson(d));
}

// --- Loading ---

CodeCache::CodeCache(const StateDir& state, const CodeTree& code, ScriptHost& scripts, CodeCacheOptions options)
    : state_(state), code_(code), scripts_(scripts), options_(std::move(options)) {}

Result<CodeLoad> CodeCache::load(const std::string& target) { return load(&target); }

Result<CodeLoad> CodeCache::loadFull() { return load(nullptr); }

Result<CodeLoad> CodeCache::load(const std::string* target) {
    auto st = readState();
    if (!st) return st.error();
    State state = std::move(st).value();

    auto listing = code_.list();
    if (!listing) {
        if (state.current) return serve(state.current, target);   // keep serving what we have
        return listing.error();
    }
    std::optional<int64_t> deploy = code_.deployTrigger();
    std::string key = codeFingerprint(listing.value(), deploy, options_.dialect);
    int64_t now = options_.clock->nowMillis();

    // Whether to compile a new version now.
    bool compileNow = false;
    if (!state.current) {
        compileNow = key != state.failed;   // the first version (or a new try)
    } else if (key != state.key && key != state.failed) {
        bool deployed = deploy && *deploy != state.deploy;
        if (deployed || (state.pending == key && now - state.pendingSince >= options_.quietMillis)) {
            compileNow = true;
        } else if (state.pending != key) {
            // A change, seen for the first time: wait for quiet. If another
            // process is compiling, the next request will record it.
            auto lockFile = state_.lockFile("cgi-compile");
            if (!lockFile) return lockFile.error();
            auto lock = FileLock::tryAcquire(std::move(lockFile).value());
            if (lock && lock.value()) {
                if (auto fresh = readState(); fresh && fresh.value().pending != key) {
                    State updated = std::move(fresh).value();
                    updated.pending = key;
                    updated.pendingSince = now;
                    (void)writeState(updated);
                }
            }
        }
    }

    if (compileNow) {
        auto lockFile = state_.lockFile("cgi-compile");
        if (!lockFile) return lockFile.error();
        std::optional<FileLock> lock;
        if (state.current) {
            // Another process is compiling: serve the current version meanwhile.
            auto taken = FileLock::tryAcquire(std::move(lockFile).value());
            if (!taken) return taken.error();
            lock = std::move(taken).value();
        } else {
            // No version yet: wait for whoever is making one.
            auto taken = FileLock::acquire(std::move(lockFile).value());
            if (!taken) return taken.error();
            lock = std::move(taken).value();
        }
        if (lock) {
            auto fresh = readState();
            if (!fresh) return fresh.error();
            state = std::move(fresh).value();
            if (state.key != key && state.failed != key) return compileNew(listing.value(), key, state);
        }
    }

    if (!state.current) {
        return makeError(ErrorCode::Invalid, "the app doesn't compile; see private/state/logs/");
    }
    return serve(state.current, target);
}

Result<CodeLoad> CodeCache::compileNew(const CodeListing& listing, const std::string& key, State& state) {
    uint64_t generation = std::max(state.current, state.lastGeneration) + 1;
    state.lastGeneration = generation;
    auto refuse = [&](std::vector<Diagnostic> diagnostics) -> Result<CodeLoad> {
        (void)state_.removeTree(versionFolder(generation));
        state.failed = key;
        state.pending.clear();
        (void)writeState(state);
        if (!state.current) {
            std::string message = "the app doesn't compile";
            for (const Diagnostic& d : diagnostics) {
                if (d.severity == Diagnostic::Severity::Error) message += "\n" + d.toString();
            }
            return makeError(ErrorCode::Invalid, message);
        }
        auto load = serve(state.current, nullptr);
        if (load) load.value().failed = std::move(diagnostics);
        return load;
    };

    if (Status s = state_.saveCodeSnapshot(code_, listing, snapshotFolder(generation)); !s) {
        (void)state_.removeTree(versionFolder(generation));
        return s.error();
    }
    auto snapshot = state_.openCodeSnapshot(snapshotFolder(generation));
    if (!snapshot) return snapshot.error();

    CompileOptions options;
    options.generation = generation;
    options.scripts = &scripts_;
    options.evaluator = &scripts_;
    options.dialect = options_.dialect;
    CompileOutput out = compile(snapshot.value(), options);
    if (!out.ok()) return refuse(out.diagnostics);
    if (options_.beforeInstall) {
        if (Status s = options_.beforeInstall(out.app); !s) {
            return refuse({{Diagnostic::Severity::Error, "code", 0, s.error().message}});
        }
    }

    Datum::MapItems handlers;
    for (const auto& [path, h] : out.handlers) handlers[path.str()] = handlersToDatum(h);
    Datum::MapItems routes;
    for (const auto& [path, file] : codeFiles(*out.app)) routes[path] = Datum::string(file);
    Datum version = Datum::map({{"generation", Datum::number(static_cast<double>(generation))},
                                {"handlers", Datum::map(std::move(handlers))},
                                {"routes", Datum::map(std::move(routes))}});
    if (Status s = state_.write(versionFile(generation), toJson(version)); !s) {
        (void)state_.removeTree(versionFolder(generation));
        return s.error();
    }

    int64_t now = options_.clock->nowMillis();
    if (state.current) state.retired.emplace_back(state.current, now);
    state.current = generation;
    state.key = key;
    std::optional<int64_t> deploy = code_.deployTrigger();
    state.deploy = deploy ? *deploy : -1;
    state.pending.clear();
    state.failed.clear();
    removeRetired(state);
    if (Status s = writeState(state); !s) return s.error();

    CodeLoad load;
    load.app = out.app;
    load.code = std::move(snapshot).value();
    load.generation = generation;
    load.compiled = true;
    return load;
}

void CodeCache::removeRetired(State& state) const {
    int64_t now = options_.clock->nowMillis();
    std::vector<std::pair<uint64_t, int64_t>> kept;
    for (const auto& [generation, at] : state.retired) {
        if (now - at >= options_.retireMillis) {
            (void)state_.removeTree(versionFolder(generation));
        } else {
            kept.emplace_back(generation, at);
        }
    }
    state.retired = std::move(kept);
    // Versions that never served (failed, or left by a crash) and aren't
    // retired: anything else in cgi/ that isn't current.
    auto entries = state_.list(RelPath::parse("cgi").value());
    if (!entries) return;
    for (const DirEntry& e : entries.value()) {
        if (!e.isFolder || e.name.size() < 2 || e.name[0] != 'v') continue;
        uint64_t g = std::strtoull(e.name.c_str() + 1, nullptr, 10);
        bool keep = g == state.current;
        for (const auto& [r, at] : state.retired) keep = keep || r == g;
        if (!keep) (void)state_.removeTree(versionFolder(g));
    }
}

Result<CodeLoad> CodeCache::serve(uint64_t generation, const std::string* target) {
    auto text = state_.read(versionFile(generation), kMaxVersionBytes);
    if (!text) return text.error();
    auto version = parseJson(text.value());
    if (!version) return version.error();
    std::map<RelPath, PageHandlers> known;
    if (const Datum* handlers = field(version.value(), "handlers"); handlers && handlers->kind() == Datum::Kind::Map) {
        for (const auto& [path, d] : handlers->asMap()) {
            auto rel = RelPath::parse(path);
            auto h = handlersFromDatum(d);
            if (!rel || !h) return makeError(ErrorCode::Invalid, "private/state/" + versionFile(generation).str() + ": damaged");
            known.emplace(std::move(rel).value(), std::move(*h));
        }
    }

    // The .ms file the request's route runs, if any.
    std::set<RelPath> only;
    if (target) {
        auto path = canonicalUrlPath(*target);
        const Datum* routes = field(version.value(), "routes");
        if (path && routes) {
            if (auto file = stringField(*routes, path.value().path)) {
                if (auto rel = RelPath::parse(*file)) only.insert(std::move(rel).value());
            }
        }
    } else {
        for (const auto& [path, h] : known) only.insert(path);
    }

    auto snapshot = state_.openCodeSnapshot(snapshotFolder(generation));
    if (!snapshot) return snapshot.error();
    CompileOptions options;
    options.generation = generation;
    options.scripts = &scripts_;
    options.evaluator = &scripts_;
    options.dialect = options_.dialect;
    options.knownHandlers = &known;
    options.compileOnly = std::move(only);
    CompileOutput out = compile(snapshot.value(), options);
    if (!out.ok()) {
        std::string message = "version " + std::to_string(generation) + " no longer compiles";
        for (const Diagnostic& d : out.diagnostics) message += "\n" + d.toString();
        return makeError(ErrorCode::Invalid, message);
    }
    CodeLoad load;
    load.app = out.app;
    load.code = std::move(snapshot).value();
    load.generation = generation;
    return load;
}

}  // namespace fourfold

#include "cgi/cgi_app.h"

#include <future>
#include <sys/stat.h>

#include "appdata/migrator.h"

namespace fourfold {

const char* const kCgiModeNotice =
    "CGI mode: SSE push unavailable (pages poll instead); jobs run only when cron runs `fourfold run-jobs`; "
    "OS-level partition enforcement unavailable (the engine runs as the site's user)";

namespace {

// Waits for a run's outcome (a migration's).
Status waitForRun(const std::function<void(ScriptHost::Done)>& start) {
    auto finished = std::make_shared<std::promise<HandlerOutcome>>();
    std::future<HandlerOutcome> result = finished->get_future();
    start([finished](HandlerOutcome o) { finished->set_value(std::move(o)); });
    HandlerOutcome o;
    try {
        o = result.get();
    } catch (const std::future_error&) {
        return makeError(ErrorCode::Internal, "the script host stopped");
    }
    if (o.kind != HandlerOutcome::Kind::Error) return {};
    const ScriptError& e = o.error;
    std::string where = e.file.empty() ? "" : e.file + (e.line > 0 ? ":" + std::to_string(e.line) : "") + ": ";
    return makeError(ErrorCode::Invalid, where + e.message);
}

HttpResponse unavailable() {
    HttpResponse r = HttpResponse::text(503, "Service Unavailable: the site is being updated; try again shortly.\n");
    r.headers.set("Retry-After", "10");
    return r;
}

}  // namespace

Result<std::unique_ptr<CgiApp>> CgiApp::open(const std::string& folder, CgiOptions options) {
    std::unique_ptr<CgiApp> app(new CgiApp());
    app->options_ = options;
    auto opened = AppFolder::open(folder, AppFolder::Missing::Fail);
    if (!opened) return opened.error();
    app->root_ = opened.value().root();
    auto code = opened.value().codeTree();
    auto state = opened.value().stateDir();
    auto stateForLog = opened.value().stateDir();
    auto data = opened.value().dataDir();
    auto dataForFiles = opened.value().dataDir();
    auto dropbox = opened.value().dropbox();
    auto configDir = opened.value().configDir();
    if (!code) return code.error();
    if (!state) return state.error();
    if (!stateForLog) return stateForLog.error();
    if (!data) return data.error();
    if (!dataForFiles) return dataForFiles.error();
    if (!dropbox) return dropbox.error();
    if (!configDir) return configDir.error();
    app->code_ = std::move(code).value();
    app->state_ = std::make_unique<StateDir>(std::move(state).value());
    app->dataDir_ = std::make_unique<DataDir>(std::move(data).value());
    app->data_ = std::make_unique<DataFiles>(std::move(dataForFiles).value());
    app->dropbox_ = std::make_unique<Dropbox>(std::move(dropbox).value());
    app->configDir_ = std::make_unique<ConfigDir>(std::move(configDir).value());
    app->log_ = std::make_unique<EngineLog>(std::move(stateForLog).value());
    app->folder_ = std::move(opened).value();

    auto key = loadOrCreateEngineKey(*app->state_);
    auto dbPath = app->state_->sqlitePath(RelPath::parse("state.db").value());
    if (!key) return key.error();
    if (!dbPath) return dbPath.error();
    app->engineKey_ = std::move(key).value();

    ScriptHostOptions scriptOptions;
    CgiApp* self = app.get();
    scriptOptions.printOutput = [self](const std::string& file, const std::string& text) {
        (void)self->dropbox_->append(RelPath::parse("print.log").value(), file + ": " + text + "\n");
    };
    auto scripts = ScriptHost::create(scriptOptions);
    if (!scripts) return scripts.error();
    app->scripts_ = std::move(scripts).value();

    // The config, as `fourfold serve` reads it: an invalid one is refused.
    app->sources_.dir = app->configDir_.get();
    app->sources_.mode = ConfigMode::Serve;
    app->sources_.devMailOutput = [self](const std::string& text) { self->log_->write("mail", text); };
    if (!options.serverConfig.empty()) {
        auto file = ConfigDir::readServerConfig(options.serverConfig);
        if (!file) return makeError(file.error().code, "can't read the server config: " + file.error().message);
        app->sources_.serverConfig = std::move(file).value();
    }
    app->configStamp_ = configStamp(*app->configDir_);
    ConfigLoad load = loadConfig(app->sources_, *app->scripts_);
    if (!load.ok()) {
        std::string message = "the config has errors";
        for (const Diagnostic& d : load.diagnostics) {
            app->log_->write("config", d.toString());
            message += "\n" + d.toString();
        }
        return makeError(ErrorCode::Invalid, message);
    }
    app->config_ = load.config;
    app->scripts_->setBudget(app->config_->budget);

    auto sessions = SessionStore::open(dbPath.value(), app->config_->session);
    if (!sessions) return sessions.error();
    auto auth = AuthStore::open(dbPath.value(), app->config_->auth.store);
    if (!auth) return auth.error();
    auto rates = RateStore::open(dbPath.value());
    if (!rates) return rates.error();
    auto jobQueue = JobQueue::open(dbPath.value());
    if (!jobQueue) return jobQueue.error();
    app->sessions_ = std::move(sessions).value();
    app->auth_ = std::move(auth).value();
    app->rates_ = std::move(rates).value();
    app->jobQueue_ = std::move(jobQueue).value();
    // Shared with every other process serving the app.
    const StateDir* stateDir = app->state_.get();
    app->sessionLocks_ = makeFileLockService([stateDir](const std::string& k) { return stateDir->lockFile(k); },
                                             app->scripts_->workers());
    app->schemaLock_ = std::make_unique<SchemaFileLock>(*app->state_);

    // The app's database, opened on first use (as the standalone server's).
    const DatabaseConfig& dbConfig = app->config_->db;
    if (dbConfig.dialect == SqlDialect::Postgres) {
        PostgresSettings settings = dbConfig.postgres;
        DbLimits limits = dbConfig.limits;
        size_t pool = dbConfig.poolSize;
        app->database_ = makeLazyDatabase(SqlDialect::Postgres, [settings, limits, pool] {
            return Database::openPostgres(settings, limits, pool);
        });
    } else {
        DbLimits limits = dbConfig.limits;
        size_t pool = dbConfig.poolSize;
        const DataDir* dataDir = app->dataDir_.get();
        app->database_ = makeLazyDatabase(SqlDialect::Sqlite, [dataDir, limits, pool]() -> Result<std::unique_ptr<Database>> {
            auto path = dataDir->databasePath();
            if (!path) return path.error();
            return Database::openSqlite(path.value(), limits, pool);
        });
    }
    std::string backupFolder = app->config_->backups.folder;
    if (backupFolder.empty()) {
        auto f = app->state_->folderPath(RelPath::parse("backups").value());
        if (!f) return f.error();
        backupFolder = f.value();
    }
    app->backups_ = std::make_unique<BackupService>(*app->dataDir_, *app->state_, backupFolder,
                                                    dbConfig.dialect == SqlDialect::Sqlite);
    app->authFlow_ = std::make_unique<AuthFlow>(*app->auth_, *app->sessions_, *app->sessionLocks_,
                                                app->scripts_->workers(), app->log_.get());
    app->makeCache();
    return app;
}

CgiApp::~CgiApp() {
    // The engine and the version go before the services they use.
    keptEngine_.reset();
    kept_.reset();
}

void CgiApp::makeCache() {
    CodeCacheOptions cacheOptions;
    cacheOptions.dialect = config_->db.dialect;
    cacheOptions.clock = options_.clock;
    if (options_.quietMillis) cacheOptions.quietMillis = *options_.quietMillis;
    cacheOptions.beforeInstall = [this](const std::shared_ptr<const CompiledApp>& app) { return migrate(app); };
    cache_ = std::make_unique<CodeCache>(*state_, *code_, *scripts_, cacheOptions);
    keptEngine_.reset();
    kept_.reset();
}

void CgiApp::checkConfig() {
    std::string stamp = configStamp(*configDir_);
    if (stamp == configStamp_) return;
    configStamp_ = stamp;
    ConfigLoad load = loadConfig(sources_, *scripts_);
    if (!load.ok()) {
        for (const Diagnostic& d : load.diagnostics) log_->write("config", d.toString());
        log_->write("config", "the config has errors; still using the previous one");
        return;
    }
    bool dialectChanged = load.config->db.dialect != config_->db.dialect;
    config_ = load.config;
    scripts_->setBudget(config_->budget);
    if (keptEngine_) keptEngine_->setConfig(config_);
    if (dialectChanged) makeCache();
    log_->write("config", "reloaded private/config/");
}

Status CgiApp::migrate(const std::shared_ptr<const CompiledApp>& app) {
    if (app->migrations.empty()) return {};
    Database* db = app->usesDatabase() ? database_.get() : nullptr;
    std::shared_ptr<const CompiledApp> migrating = app;
    std::shared_ptr<const AppConfig> config = config_;
    Migrator migrator(
        db, *dataDir_,
        [this, migrating, config](const CompiledApp&, const Migration& m, DbConnection* tx) {
            RunContext context;
            context.data = data_.get();
            context.db = tx;
            ScriptBudget budget = config->budget;
            budget.wallClock = config->deploy.migrationBudget;
            budget.instructions = 1'000'000'000'000ULL;   // bounded by its time instead
            context.budget = budget;
            int number = m.number;
            return waitForRun([&](ScriptHost::Done done) {
                scripts_->runMigration(migrating, number, std::move(context), std::move(done));
            });
        },
        *options_.clock);
    if (config->db.dialect == SqlDialect::Sqlite) {
        const DataDir* dataDir = dataDir_.get();
        migrator.setDatabaseExists([dataDir] {
            auto path = dataDir->databasePath();
            struct stat info;
            return path && ::stat(path.value().c_str(), &info) == 0;
        });
    }

    // Planned and applied with the schema lock held exclusively: no request
    // is running, and no other process is migrating.
    auto lock = schemaLock_->exclusive();
    if (!lock) return lock.error();
    auto applied = migrator.applied();
    if (!applied) return applied.error();
    MigrationPlan plan = planMigrations(*app, applied.value());
    for (const std::string& w : plan.warnings) log_->write("migrate", w);
    if (!plan.ok()) {
        std::string message = "this version can't run on the app's data:";
        for (const std::string& e : plan.errors) message += " " + e;
        return makeError(ErrorCode::Conflict, message);
    }
    if (plan.pending.empty()) return {};

    std::vector<int> appliedBefore;
    for (const auto& [number, record] : applied.value()) appliedBefore.push_back(number);
    auto backup = backups_->backup("before-migrations", appliedBefore);
    if (!backup) {
        return makeError(backup.error().code, "couldn't take a backup before migrating, so nothing was changed: " +
                                                  backup.error().message);
    }
    Migrator::Outcome outcome = migrator.apply(*app, plan.pending);
    if (!outcome.ok()) {
        std::string message = outcome.error->message;
        Status restored = backups_->restore(backup.value().name, false, false);
        if (restored) {
            message += " (nothing was changed: the database work was undone, and the data files restored from "
                       "backup " + backup.value().name + ")";
        } else {
            message += "; restoring the data files from backup " + backup.value().name + " failed too (" +
                       restored.error().message + "): restore that backup with `fourfold restore`";
        }
        log_->write("migrate", message);
        return makeError(ErrorCode::Invalid, "migration failed: " + message);
    }
    if (config->deploy.clearAnonymousSessions) {
        auto removed = sessions_->removeAnonymous();
        log_->write("session", removed ? "cleared " + std::to_string(removed.value()) +
                                             " anonymous sessions after migrations"
                                       : "clearing anonymous sessions failed: " + removed.error().message);
    }
    (void)backups_->prune(config->backups.keep);
    log_->write("migrate", "applied " + std::to_string(outcome.applied.size()) + " migration" +
                               (outcome.applied.size() == 1 ? "" : "s") + " (backup first: " +
                               backup.value().name + ")");
    return {};
}

void CgiApp::report(const CodeLoad& load) {
    if (!load.failed.empty()) {
        for (const Diagnostic& d : load.failed) log_->write("compile", d.toString());
        log_->write("compile", "the new code wasn't installed; version " + std::to_string(load.generation) +
                                   " is still serving");
    }
    if (load.compiled) {
        for (const Diagnostic& d : load.app->diagnostics) log_->write("compile", d.toString());
        log_->write("compile", "version " + std::to_string(load.generation) + " installed");
        log_->write("start", kCgiModeNotice);
    }
}

std::unique_ptr<Engine> CgiApp::engineFor(CodeLoad& load, bool https) {
    EngineOptions options;
    // Apache has already chosen this app by its host name; config.domain,
    // if set, narrows it further.
    if (!config_->site.domain.empty()) options.allowedHosts = {config_->site.domain};
    options.sessions = sessions_.get();
    options.sessionLocks = sessionLocks_.get();
    options.data = data_.get();
    options.dropbox = dropbox_.get();
    options.log = log_.get();
    options.auth = auth_.get();
    options.authRoutes = authFlow_.get();
    options.rates = rates_.get();
    options.db = database_.get();
    options.engineKey = engineKey_;
    options.jobs = jobQueue_.get();
    options.publishWithoutPush = true;
    options.clock = options_.clock;
    options.https = https;
    auto engine = std::make_unique<Engine>(load.app, std::move(*load.code), scripts_.get(), std::move(options));
    load.code.reset();
    engine->setConfig(config_);
    return engine;
}

Result<CodeLoad> CgiApp::loadFull() {
    auto load = cache_->loadFull();
    if (load) report(load.value());
    return load;
}

HttpResponse CgiApp::handle(const HttpRequest& request, bool https) {
    checkConfig();

    // A Fourfold server running this app as well would be a mistake (its
    // locks are in memory): refuse rather than share the data with it.
    if (auto lockFile = state_->lockFile("server")) {
        auto shared = FileLock::tryAcquire(std::move(lockFile).value(), FileLock::Mode::Shared);
        if (shared && !shared.value()) {
            log_->write("error", "a Fourfold server (fourfold serve or dev) is running this app; CGI requests are "
                                 "refused while it is");
            return unavailable();
        }
    }

    // The version to serve with: the one in memory (FastCGI), if it's
    // still current; else what the cache gives.
    Engine* engine = nullptr;
    std::unique_ptr<Engine> oneShot;
    Result<CodeLoad> load = options_.keepVersion ? cache_->loadFull() : cache_->load(request.target);
    if (!load) {
        log_->write("error", load.error().message);
        return HttpResponse::text(500, "Internal Server Error\n");
    }
    report(load.value());
    if (options_.keepVersion) {
        if (!kept_ || kept_->generation != load.value().generation || keptHttps_ != https) {
            if (!kept_ || kept_->generation != load.value().generation) kept_ = std::move(load).value();
            if (!kept_->code) {
                // The same version, over the other scheme: its snapshot again.
                auto again = cache_->loadFull();
                if (!again) {
                    log_->write("error", again.error().message);
                    return HttpResponse::text(500, "Internal Server Error\n");
                }
                kept_ = std::move(again).value();
            }
            keptEngine_ = engineFor(*kept_, https);
            keptHttps_ = https;
        }
        engine = keptEngine_.get();
    } else {
        oneShot = engineFor(load.value(), https);
        engine = oneShot.get();
    }

    auto schema = schemaLock_->enter(config_->deploy.migrationWait);
    if (!schema) {
        log_->write("error", schema.error().message);
        return HttpResponse::text(500, "Internal Server Error\n");
    }
    if (!schema.value()) return unavailable();

    std::promise<HttpResponse> done;
    std::future<HttpResponse> result = done.get_future();
    engine->handle(request, [&done](HttpResponse r) { done.set_value(std::move(r)); });
    HttpResponse response = result.get();
    if (response.stream) response = HttpResponse::text(501, "Server-sent events aren't available in CGI mode\n");
    return response;
}

}  // namespace fourfold

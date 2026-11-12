// ScriptHost: everything that runs MiniScript (Arch §3, §5 `script`).
//
// MiniScript 2's garbage collector is process-wide and unsynchronized, so
// every MiniScript value lives on one thread, the script thread, which this
// class owns. Request VMs are interleaved on it in time slices; a VM that
// waits (on a timer, or on work done by the worker pool) is parked, and the
// thread runs other VMs meanwhile.
//
// Nothing in this header mentions MiniScript types. What crosses into and out
// of the script thread is plain C++ data: CodeFile in, CompiledScripts and
// HandlerOutcome out, RequestFacts in.
//
// Only one ScriptHost may exist at a time in a process (MS2's state is
// global). Its methods may be called from any thread.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "appdata/data_files.h"
#include "appdata/database.h"
#include "base/datum.h"
#include "compiledapp/compiled_app.h"
#include "outbound/outbound_client.h"
#include "partition/code_tree.h"
#include "partition/config_dir.h"
#include "partition/data_dir.h"
#include "platform/worker_pool.h"
#include "template/template.h"

namespace fourfold {

// Per-run limits (overview §2, "Resource budgets"; Arch §1).
struct ScriptBudget {
    // Instructions per run, counted in whole slices: MS2 doesn't report how
    // many instructions a slice actually ran, so a run may stop up to one
    // slice early (never late).
    uint64_t instructions = 10'000'000;
    // Wall-clock time per run, including time parked.
    std::chrono::milliseconds wallClock{10'000};
    // The longest string or list that repetition or concatenation may build
    // (MS2's MAX_COLLECTION_SIZE). Process-wide in MS2.
    int maxCollectionSize = 16 * 1024 * 1024;
    // RawData buffers live outside MS2's heap: the largest one (the same as
    // the largest file in data/), and all of one run's together.
    size_t maxRawDataBytes = 16 * 1024 * 1024;
    size_t maxRawDataTotal = 64 * 1024 * 1024;
};

struct ScriptHostOptions {
    ScriptBudget budget;
    // Instructions per time slice: how long one VM runs before the next gets
    // a turn.
    uint32_t sliceInstructions = 10'000;
    // Threads for work that would block the script thread.
    size_t workerThreads = 4;
    // Threads for work that waits on other servers (outbound HTTP, mail),
    // kept apart so a slow API can't hold up the rest. Calls past this many
    // at once wait their turn (within their run's time budget).
    size_t networkThreads = 8;
    // Registers the _test* intrinsics (for tests and sample apps).
    bool testIntrinsics = false;
    // Where `print` goes: the file that printed, and the text. Default: dropped.
    std::function<void(const std::string& file, const std::string& text)> printOutput;
};

// A signed-in user, as handlers see it (`req.user`, overview §5, The user
// object) and as the `users` module returns it.
struct UserFacts {
    std::string id;       // stable and opaque
    std::string name;
    std::string email;
    std::vector<std::string> roles;   // sorted

    bool operator==(const UserFacts& o) const {
        return id == o.id && name == o.name && email == o.email && roles == o.roles;
    }
};

// An uploaded file (overview §4, Forms and validation), as `req.files`
// shows it: checked by content against its field's declared types.
struct UploadFacts {
    std::string field;          // the form field's name
    std::string filename;       // as the browser sent it, cleaned: no path, no control characters
    std::string contentType;    // detected from the content ("image/png")
    std::shared_ptr<const std::string> bytes;
};

// What a handler sees of a request (`req`, overview §4). Plain data, built by
// the pipeline; the script thread turns it into a frozen map.
struct RequestFacts {
    using Pairs = std::vector<std::pair<std::string, std::string>>;

    std::string method;    // "GET"
    std::string path;      // canonical, "/guestbook"
    Pairs query;           // decoded; for a repeated name, the first wins
    Pairs headers;         // names lowercased; repeated names joined with ", "
    // The submitted form (url-encoded or multipart), as raw strings, when the
    // handler declares no form; for a repeated name, the first wins.
    Pairs form;
    // With a form declaration (overview §4, Forms and validation): the
    // declared fields only, normalized and typed (`req.form`); the errors
    // by field (`req.errors`; nullopt: none); and the uploads (`req.files`).
    std::optional<Datum> declaredForm;
    std::optional<Datum> errors;
    std::vector<UploadFacts> files;
    // What the visitor typed in each declared field, normalized but not
    // converted, for reshow(): a string, or a list for `choices`.
    Datum typed = Datum::map({});
    std::optional<UserFacts> user;   // the signed-in visitor, looked up for this request
    // "html", or "markdown" for a Markdown page's /x.md URL (`req.format`).
    std::string format = "html";
    // The visitor's address (`req.ip`): the connection's, or, from a trusted
    // proxy, the one it forwarded.
    std::string ip;
    // A webhook's request only (overview §6, Inbound webhooks): the body as
    // sent (`req.rawBody`), for checking the caller's signature; and, if
    // its Content-Type is JSON, the body parsed (`req.json`; null if it
    // isn't valid JSON).
    std::optional<std::string> rawBody;
    std::optional<Datum> json;
};

// Counting for rate limits from code (`ratelimit`, overview §5): the
// engine's RateStore. Blocks (SQLite), so the module calls it on the worker
// pool; it may be called from several threads at once.
class RateCounter {
public:
    virtual ~RateCounter() = default;
    // Counts one request against `key` under `rule` (rule.key is ignored);
    // whether it's within the limit. An error counts as within it.
    virtual bool hit(const std::string& key, const RateRule& rule) = 0;
};

// The engine's users, for the `users` module (overview §5, The user object):
// app code may look users up and grant or revoke roles (it's trusted, per
// the threat model). Implemented by the engine over its AuthStore. Every
// method blocks, so the module calls them on the worker pool; they may be
// called from several threads at once.
class UserDirectory {
public:
    virtual ~UserDirectory() = default;
    virtual Result<std::optional<UserFacts>> get(const std::string& id) = 0;
    virtual Result<std::optional<UserFacts>> findByEmail(const std::string& email) = 0;
    // NotFound if there's no such user. Granting a role the user has, or
    // revoking one they don't, is fine.
    virtual Status grant(const std::string& id, const std::string& role) = 0;
    virtual Status revoke(const std::string& id, const std::string& role) = 0;
};

// One recipient of app mail: one of the app's users (by id), or an address
// code/outbound.ms allows.
struct AppMailRecipient {
    std::string userId;    // one of these two is set
    std::string address;   // lowercased
};

// A message from the `mail` module (overview §6, Email from app code),
// checked and rendered.
struct AppMailMessage {
    AppMailRecipient to;
    std::vector<AppMailRecipient> bcc;   // blind copies: in no header
    std::string subject;
    std::string text;        // the template's Markdown
    std::string html;        // a whole HTML document
};

// App mail, for the `mail` module: implemented by the engine, which finds a
// user's address, counts the app's hourly limit, and sends through the
// configured transport. Blocks (the network), so the module calls it on the
// worker pool; it may be called from several threads at once.
class AppMailer {
public:
    virtual ~AppMailer() = default;
    // The address the message went to, or why it wasn't sent. Nothing is
    // sent unless every recipient can be.
    virtual Result<std::string> send(const AppMailMessage& message) = 0;
};

// A job enqueued by a run (overview §2, Enqueuing): committed to the queue
// only if the run succeeds.
struct JobRequest {
    std::string name;      // the job: job.<name> in code/jobs/
    Datum args = Datum::map({});   // inert
    std::optional<std::string> key;   // unique while pending
    int64_t delayMs = 0;   // run no sooner than this long after the commit
};

// A message for a push channel's subscribers (overview §7, Real-time push),
// from `publish`: sent only if the run succeeds.
struct PushMessage {
    std::string channel;   // "orders", for /live/orders
    std::string event;     // the SSE event name ("message" unless given)
    std::string data;      // HTML (a string's text, escaped)
};

// The visitor's session as a run starts (overview §2, Session storage).
struct SessionInput {
    Datum app = Datum::map({});      // the `session` map
    // The engine's part, out of the app's reach: form state kept for one
    // showing by reshow() (under "forms").
    Datum engine = Datum::map({});
};

// What a run did to the session, for the engine to save after it.
struct SessionResult {
    // The `session` map at the end, as canonical JSON, checked to be inert
    // and within the size cap.
    // A page with a {{flash}} field was rendered, so `session.flash` (if
    // any) has been shown, and is left out of `app`.
    std::string app;
    // The engine's part as canonical JSON, if the run changed it (reshow()
    // stored a form's state, or formState() used it up).
    std::optional<std::string> engine;
};

// The engine's side of a run's session: loading it (after taking its lock).
// The script host calls open() once, on the script thread, before the
// page's code runs; the run waits, parked, under its wall-clock budget,
// until `ready` is called (from any thread). Saving is the engine's, after
// the run.
class SessionAccess {
public:
    virtual ~SessionAccess() = default;
    virtual void open(std::function<void(Result<SessionInput>)> ready) = 0;
};

// Proof that the policy gate allowed a request (Arch §7, "Access is checked
// before any handler"): ScriptHost::run requires one, and only PolicyGate can
// make one. (Tests make theirs through TestGate, in tests/support.)
class GateApproval {
private:
    GateApproval() = default;
    friend class PolicyGate;
    friend struct TestGate;
};

// Everything one run may use (Arch §5, "The RunContext"): the request, and
// the services the engine gives this kind of run. A module whose service is
// null isn't available to the run (its functions return an error).
struct RunContext {
    RequestFacts request{};
    // The response's CSP nonce and the Markdown stylesheet, for the page and
    // every frag() the run renders.
    RenderOptions render{};
    // Set for a route that uses `session` (its code names it, or its page
    // has a {{flash}} field). Must stay alive
    // until the run is done.
    std::shared_ptr<SessionAccess> session{};
    // The largest `session` map, serialized; over it, the run fails.
    size_t sessionMaxBytes = 64 * 1024;
    // The app's data/ (the `data` module). Must outlive the run.
    DataFiles* data = nullptr;
    // The app's dropbox/ (the `dropbox` module). Must outlive the run.
    const Dropbox* dropbox = nullptr;
    // The engine's users (the `users` module). Must outlive the run.
    UserDirectory* users = nullptr;
    // Rate limits from code (`ratelimit`). Must outlive the run. Null: every
    // call is within the limit.
    RateCounter* rates = nullptr;
    // Who a `per ip`, `per user` or `per session` rate counts this visitor
    // as: their address group, user id (falling back to the address group),
    // and session (likewise).
    std::string rateIp{}, rateUser{}, rateSession{};
    // A handler with a form declaration: the declaration (for reshow), and
    // the URL of its file's page, where reshow sends a visitor without
    // JavaScript.
    std::shared_ptr<const FormSpec> form{};
    std::string pageUrl{};
    // The app's database (the `db` module), and the queries db.query may
    // name: the app's, or a migration's own (set by runMigration). Must
    // outlive the run. Null: no database.
    QueryRunner* db = nullptr;
    const QuerySet* queries = nullptr;
    // Limits for this run instead of the host's (a migration's budget).
    std::optional<ScriptBudget> budget{};
    // Outbound HTTP and secrets (the `http`, `secrets` and `hmac` modules,
    // overview §6): the app's client, with the config's settings and
    // secrets. Null: none of them (migrations, or no client).
    std::shared_ptr<OutboundClient> outbound{};
    // App mail (the `mail` module). Null: none.
    std::shared_ptr<AppMailer> mailer{};
    // `enqueue` and `publish` (overview §2, §7): whether the engine takes
    // the run's jobs and push messages (when it succeeds). Off: they return
    // an error.
    bool enqueue = false;
    bool publish = false;
};

// A compile or runtime error in a script, located for the developer.
struct ScriptError {
    std::string message;    // "Undefined Identifier: 'x' is unknown in this context"
    std::string file;       // "code/pages/hello.ms"; empty if unknown
    int line = 0;           // 0 if unknown
};

// What a handler run produced (overview §4, Return values), converted to
// plain data before it leaves the script thread.
struct HandlerOutcome {
    enum class Kind {
        Render,      // render returned a map: `body` is its page, filled in (as HTML or Markdown)
        Fragment,    // frag(...), markdown(...) or unsafeHTML(...): `body` is the HTML
        Text,        // a string: text/plain
        Redirect,    // redirect(url): 303
        Json,        // json(value)
        Status,      // status(code, body), or reshow(...) with HTMX (422)
        NoContent,   // null: 204
        Error,       // an error value, a runtime error, or a budget overrun: 500
    };

    Kind kind = Kind::NoContent;
    std::string body;          // Render, Fragment, Text, Json, Status
    // Status, when it has a body; Render and Fragment, when the body is
    // Markdown (kMarkdownContentType). Empty: HTML.
    std::string contentType;
    // Fragment, or Status with an HTML body: it contains a <script>, which
    // can't be sent on its own (it wouldn't have the page's CSP nonce).
    bool hasScript = false;
    // Missing and unused fields, from rendering the page or any fragment.
    std::vector<RenderWarning> warnings;
    std::string location;      // Redirect
    int status = 0;            // Status
    // Status, Text, Fragment: more response headers (reshow's HX-Retarget;
    // file.send's Content-Disposition).
    std::vector<std::pair<std::string, std::string>> headers;
    ScriptError error;         // Error
    // If the run had session access and its handler finished without an
    // error: what it did to the session.
    std::optional<SessionResult> session;
    // Unless the run failed (Error): the jobs it enqueued and the push
    // messages it published, for the engine to commit.
    std::vector<JobRequest> enqueued;
    std::vector<PushMessage> published;
    // If the run failed: the names of the jobs it had enqueued, which are
    // dropped (for a dev-mode note).
    std::vector<std::string> droppedJobs;
};

// The handlers found in one page's .ms file, by running its top level once at
// compile time (overview §4, Handler model).
struct PageHandlers {
    bool render = false;
    std::vector<std::string> get;    // the keys of `get`, sorted
    std::vector<std::string> post;   // the keys of `post`, sorted
    std::vector<std::string> webhook;   // the keys of `webhook`, sorted
    std::vector<std::string> job;       // a file in code/jobs/: the keys of `job`, sorted
    // The file, or a code/lib/ file it imports, mentions `session`: its
    // runs get the session (loaded and locked). Requests to
    // pages that don't never wait for a session's lock.
    bool usesSession = false;
    // The file's `access =` declaration (overview §5, Access control), as
    // its top level left it; nullopt if it set none. The compiler checks it.
    std::optional<Datum> access;
    // The file's form declarations (overview §4, Forms and validation),
    // form.x by x, as its top level left them. The compiler checks them.
    std::map<std::string, Datum> forms;
};

struct CompiledScripts {
    // Null if any file failed to compile.
    std::shared_ptr<const ScriptBundle> bundle;
    std::map<RelPath, PageHandlers> pages;   // by the page file's path ("pages/hello.ms")
    std::vector<Diagnostic> diagnostics;
};

// A migration's MiniScript (overview §3, Migration files): a single .ms
// migration, or a folder migration's migrate.ms and its helpers. It can
// import only its helpers, never code/lib/; a single file imports nothing,
// and can't name `db`.
struct MigrationScript {
    int number = 0;
    std::string name;                 // "003_backfill_entry_time"
    CodeFile main;                    // migrate.ms, or the single .ms file
    std::vector<CodeFile> helpers;    // the folder's other .ms files, importable by name
    bool folder = false;
};

// Every .ms file the compiler hands the script side.
struct ScriptSources {
    // .ms files in code/pages/, and those directly in code/lib/ (importable
    // by name: "util" for lib/util.ms).
    std::vector<CodeFile> pages;
    std::vector<CodeFile> libs;
    std::vector<MigrationScript> migrations;
    std::optional<CodeFile> seed;     // code/seed.ms (it may import code/lib/)
    // .ms files in code/jobs/ (overview §2, Defining jobs): each defines
    // functions in a `job` map, which only these files have. Found like
    // pages' handlers, in CompiledScripts::pages by path ("jobs/mail.ms").
    std::vector<CodeFile> jobs{};
};

// The compiler's view of the script side: .ms files in, a bundle out.
class ScriptCompiler {
public:
    virtual ~ScriptCompiler() = default;

    virtual CompiledScripts compileScripts(const ScriptSources& sources) = 0;

    // Pages and libs only.
    CompiledScripts compileScripts(const std::vector<CodeFile>& pages, const std::vector<CodeFile>& libs) {
        return compileScripts(ScriptSources{pages, libs, {}, std::nullopt, {}});
    }
};

// The result of evaluating a file as data.
struct EvaluatedData {
    Datum value;                        // the named global; null if the file didn't set it
    std::optional<ScriptError> error;   // set if the file failed, or its value isn't inert
};

// Runs declaration and config files as data (overview §5, §10; Arch §5
// script): in a restricted VM with no data, session, files, imports or
// waiting, and a small budget. The only output is one global, which must be
// inert (numbers, strings, lists and maps). Files come only as CodeFile
// (code/access.ms) or ConfigFile (private/config/), never from data/.
class DataEvaluator {
public:
    virtual ~DataEvaluator() = default;

    // Waits for the result: don't call from the script thread.
    virtual EvaluatedData evaluate(const CodeFile& file, const std::string& global) = 0;
    virtual EvaluatedData evaluate(const ConfigFile& file, const std::string& global) = 0;
};

class ScriptHost : public ScriptCompiler, public DataEvaluator {
public:
    using Done = std::function<void(HandlerOutcome)>;

    // Starts the script thread and the worker pool. Fails (Conflict) if
    // another ScriptHost exists.
    static Result<std::unique_ptr<ScriptHost>> create(ScriptHostOptions options = {});
    // Stops the script thread. Runs still in progress are abandoned, and their
    // Done callbacks never called.
    ~ScriptHost() override;

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Compiles on the script thread, and waits for the result.
    CompiledScripts compileScripts(const ScriptSources& sources) override;
    using ScriptCompiler::compileScripts;

    // DataEvaluator.
    EvaluatedData evaluate(const CodeFile& file, const std::string& global) override;
    EvaluatedData evaluate(const ConfigFile& file, const std::string& global) override;

    // Runs `handler` from `app` for one request, in a fresh VM, with the
    // request and services in `context`. `done` is called exactly once, on
    // the script thread, unless the host is destroyed first. `app` stays
    // pinned (with its bytecode) until then.
    void run(std::shared_ptr<const CompiledApp> app, HandlerRef handler, const GateApproval& approval,
             RunContext context, Done done);

    // Runs job `job` (a HandlerRef of kind Job) of `app` (overview §2,
    // Background jobs): its file's top level, then the job's function, with
    // `args` as a frozen map, in a fresh VM. There's no request or session;
    // the context gives it the services jobs have, and its budget. `done`
    // gets NoContent, or Error if the job failed (a runtime error, its
    // budget ran out, or it returned an error value); with the jobs it
    // enqueued and the messages it published, unless it failed.
    void runJob(std::shared_ptr<const CompiledApp> app, HandlerRef job, Datum args, RunContext context, Done done);

    // Runs migration `number` of `app` (overview §3, Migration files): its
    // file's top level, then migrate(), in a fresh VM. The context gives it
    // data/ and the database (a connection in the migration's transaction);
    // its db.query reaches only the migration's own queries, and it gets no
    // session, users, mail or the like. `done` gets NoContent, or Error if
    // the migration failed (an error, or migrate returned an error value).
    void runMigration(std::shared_ptr<const CompiledApp> app, int number, RunContext context, Done done);
    // Runs code/seed.ms's top level (overview §3, Dev mode), with the app's
    // libs and queries.
    void runSeed(std::shared_ptr<const CompiledApp> app, RunContext context, Done done);

    // Changes the budget for runs started from now on (config reloads).
    void setBudget(ScriptBudget budget);

    // The pool that does work that would block the script thread. Engines
    // may share it (e.g. to save sessions).
    WorkerPool& workers();

    // For tests and measurements; each call runs on the script thread and waits.
    struct Stats {
        size_t activeRuns = 0;     // started, not yet done
        size_t parkedRuns = 0;     // waiting on a timer or the worker pool
        int liveMaps = 0;          // MS2 GC counts, after a full collection
        int liveLists = 0;
        int liveFunctions = 0;
        int liveBundles = 0;       // compiled .ms files of app generations not yet released
    };
    Stats stats(bool collectFirst = true);

    struct Impl;   // private

private:
    explicit ScriptHost(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace fourfold

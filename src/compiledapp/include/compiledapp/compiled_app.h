// CompiledApp: an immutable description of one compiled version of the app
// (Arch §6). It's the only thing the engine knows about the app's code.
// Plain data and lookups, no logic; the compiler builds it.
//
// It holds routes, static files, templates (pages and fragments, HTML and
// Markdown), and the compiled scripts. Queries, access rules and the rest arrive
// with their milestones.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compiledapp/outbound_policy.h"
#include "compiledapp/schedule.h"
#include "compiledapp/sql.h"
#include "partition/paths.h"
#include "template/template.h"

namespace fourfold {

// The compiled .ms files of one app generation. Defined by the script
// component, which owns the bytecode (it lives in MiniScript's GC, on the
// script thread); to everything else it's opaque (Arch §6, "The script half").
class ScriptBundle;

struct Diagnostic {
    enum class Severity { Error, Warning };

    Severity severity;
    std::string file;   // "code/pages/about.html", or "code" for the whole tree
    int line = 0;       // 0 if it isn't about a particular line
    std::string message;

    // "code/pages/about.html:3: error: message"
    std::string toString() const;
};

enum class RouteKind {
    Page,      // a page: an .html template, its .ms file's render, or both
    Static,    // a static file, served with its content type from StaticIndex
    Handler,   // get.x and/or post.x in a page's .ms file
    Markdown,  // a .md page: as HTML in its layout, or as Markdown at /x.md
};

const char* routeKindName(RouteKind kind);

// A function in a page's .ms file that a URL can reach (overview §4,
// Handler model): the global `render`, or a function in the `get`, `post`
// or `webhook` map. A webhook (overview §6, Inbound webhooks) is a POST
// handler without CSRF protection, for callers that aren't browsers. Or a
// job (overview §2, Background jobs): a function in the `job` map of a file
// in code/jobs/, which no URL reaches.
struct HandlerRef {
    enum class Kind { Render, Get, Post, Webhook, Job };

    Kind kind = Kind::Render;
    RelPath source;      // "pages/guestbook.ms", "jobs/mail.ms"
    std::string name;    // "render", or the key in get/post ("sign" for post.sign)

    // "render", "get.more", "post.sign", "job.sendWelcome"
    std::string label() const;
};

// Who may use a route (overview §5, Access control), already combined from
// every rule that applies: each requirement in `all` must be met (AND), and
// a requirement is met by any of its roles (OR). A requirement with no
// roles is "login": any signed-in user. No requirements: public.
struct AccessLevel {
    std::vector<std::vector<std::string>> all;   // each sorted, no repeats

    bool isPublic() const { return all.empty(); }
    // Adds a requirement (empty: "login"). Only ever tightens: a requirement
    // already implied by another (a superset of roles, or "login" once a
    // role is required) is dropped.
    void require(std::vector<std::string> anyOf);
    // Whether a visitor with these roles may use the route; `signedIn` false
    // means not signed in (no roles).
    bool allows(bool signedIn, const std::vector<std::string>& roles) const;
    // "public", "login", "admin", "admin or moderator", "login and editor"...
    std::string describe() const;

    bool operator==(const AccessLevel& o) const { return all == o.all; }
};

// --- Forms (overview §4, Forms and validation) ---

enum class FormFieldType { Text, Multiline, Email, Url, Number, Int, Bool, Choice, Choices, Date, File };

// "text", "multiline", ...; and the reverse (nullopt for an unknown name).
const char* formFieldTypeName(FormFieldType type);
std::optional<FormFieldType> formFieldTypeFrom(const std::string& name);

// One declared field: its type and rules.
struct FormField {
    std::string name;
    FormFieldType type = FormFieldType::Text;
    bool required = false;
    // Text types: the length, in characters. Number and int: the value.
    std::optional<double> min;
    std::optional<double> max;
    std::optional<int> maxLinks;           // text types: URLs allowed in the value
    std::vector<std::string> options;      // choice and choices
    double maxKB = 0;                      // file: the largest upload, in KB (1024 bytes)
    std::vector<std::string> types;        // file: content types, checked by content ("image/png", "image/*")
    std::map<std::string, std::string> messages;   // by rule ("required", "max", ...): the message to show
};

// A form declaration, form.x in a page's .ms file: the fields post.x
// accepts. Read at compile time; the request path only looks fields up.
struct FormSpec {
    RelPath source;      // "pages/guestbook.ms"
    std::string name;    // "sign"
    std::vector<FormField> fields;   // in a stable order (by name)

    const FormField* find(const std::string& field) const;
    bool hasFiles() const;
    // The most a submission's uploads may add up to, in bytes.
    size_t maxUploadBytes() const;
    // "form.sign"
    std::string label() const { return "form." + name; }
    // The key for CompiledApp::forms: "pages/guestbook.ms#sign".
    std::string key() const { return source.str() + "#" + name; }
};

// The content types a file field may declare (checked by content, so only
// those Fourfold can recognize): image/png, image/jpeg, image/gif,
// image/webp, application/pdf, application/zip, text/plain, and image/*.
bool isCheckableUploadType(const std::string& type);

// --- Rate limits (overview §5, Rate limits) ---

// One limit: at most `count` requests per `seconds`, counted per visitor
// address group, per signed-in user (falling back to the address for
// visitors who aren't), or per session (falling back to the address).
struct RateRule {
    enum class Per { Ip, User, Session };

    int64_t count = 0;
    int64_t seconds = 60;
    Per per = Per::Ip;
    // What's counted together: the rule's source ("default", or
    // "code/limits.ms: /guestbook/sign"); every route a rule covers shares
    // its counter.
    std::string key;

    // "5/minute per ip"
    std::string describe() const;
    bool operator==(const RateRule& o) const {
        return count == o.count && seconds == o.seconds && per == o.per && key == o.key;
    }
};

// A rate as written: "5/minute", "100 per minute per user",
// "10/hour/session". nullopt, with `error` set, if it isn't one.
std::optional<RateRule> parseRate(const std::string& text, std::string& error);

// A role name: a letter, then letters, digits, '_' or '-', at most 64
// bytes; and not "login" or "public", which mean something else in access
// rules.
bool isValidRoleName(const std::string& role);

struct RouteEntry {
    RouteKind kind;
    // The file in code/ this route comes from: the .html for a page that has
    // one (otherwise its .ms), the static file, or the .ms for a handler.
    RelPath source;
    // The route's one canonical URL ends in '/': true for a folder's index
    // page (/docs/ for pages/docs/index.html), so relative links in it
    // resolve inside the folder. The other form redirects to this one.
    bool trailingSlash = false;
    std::optional<HandlerRef> render = std::nullopt;   // Page: its .ms file's render, if any
    std::optional<HandlerRef> get = std::nullopt;      // Handler: get.x, if any
    std::optional<HandlerRef> post = std::nullopt;     // Handler: post.x, if any
    // Handler: webhook.x, if any (never with post.x): POST without CSRF
    // protection, with the body as `req.rawBody`.
    std::optional<HandlerRef> webhook = std::nullopt;
    // The route's code uses the session (its .ms file, or a lib it imports,
    // names `session` or `flash`), or its page has a {{flash}} field: the
    // session is loaded, and locked while its code runs.
    bool usesSession = false;
    // Who may use it (overview §5), from code/access.ms and the page's
    // `access =`; `accessSources` names each rule that applied
    // ("code/access.ms: /admin", "code/pages/admin/index.ms").
    AccessLevel access{};
    std::vector<std::string> accessSources{};

    // Rate limits (overview §5) from code/limits.ms, each counting every
    // request to the route; the engine's default limits (for every method
    // but GET) apply as well. `limitSources` names each rule that applied.
    std::vector<RateRule> limits{};
    std::vector<std::string> limitSources{};
    // The timing check: a submission (any method but GET) must come at least
    // this long after its form was rendered, and, if maxAgeSeconds is set,
    // no longer. 0: off.
    double minSeconds = 0;
    double maxAgeSeconds = 0;
    // Proof of work: a submission must carry a solved, unused puzzle.
    bool challenge = false;

    // Handler: the form post.x declares (form.x), if any.
    std::shared_ptr<const FormSpec> form{};
    // Handler: the URL of its file's own page (/guestbook for
    // pages/guestbook.ms), where reshow sends a visitor without
    // JavaScript; empty if the file has no page.
    std::string pageUrl{};
};

// Canonical URL path (see canonicalUrlPath, which reports the trailing slash
// separately) → route. Lookups are exact: no patterns, no case folding
// (overview §5).
//
// A Markdown page's route also answers at its Markdown URL (/about.md for
// /about; overview §4, Markdown as Markdown): an alias for the same entry,
// so the same access rules, gate and code apply.
class RouteTable {
public:
    // False if `path` is already taken (by a route or an alias).
    bool add(std::string path, RouteEntry entry);
    const RouteEntry* find(const std::string& path) const;
    // For the compiler, while it builds the table.
    RouteEntry* findForUpdate(const std::string& path);

    // False if `alias` is already taken, or `path` isn't a route.
    bool addMarkdownAlias(std::string alias, std::string path);
    // The route `alias` is the Markdown URL of, or null; `path` gets its path.
    const RouteEntry* findMarkdown(const std::string& alias, std::string* path = nullptr) const;
    // Alias → route path.
    const std::map<std::string, std::string>& markdownAliases() const { return aliases_; }

    size_t size() const { return routes_.size(); }
    std::map<std::string, RouteEntry>::const_iterator begin() const { return routes_.begin(); }
    std::map<std::string, RouteEntry>::const_iterator end() const { return routes_.end(); }

private:
    std::map<std::string, RouteEntry> routes_;
    std::map<std::string, std::string> aliases_;
};

// A Markdown page's Markdown URL: /about.md for /about, /docs/index.md for
// /docs/ (`trailingSlash`), and /index.md for /.
std::string markdownUrlFor(const std::string& path, bool trailingSlash);

struct StaticFile {
    std::string contentType;   // "image/png", "text/css; charset=utf-8"
    uint64_t size = 0;
    int64_t modifiedNs = 0;
};

// The static files in code/pages/, by source path.
class StaticIndex {
public:
    void add(RelPath source, StaticFile file);
    const StaticFile* find(const RelPath& source) const;
    size_t size() const { return files_.size(); }

private:
    std::map<RelPath, StaticFile> files_;
};

// The parsed templates: pages in code/pages/ by source path (.html, and .md
// as Markdown templates), and fragments in code/frags/ by name ("entry" for
// frags/entry.html or entry.md), with their includes resolved.
class TemplateSet {
public:
    void add(RelPath source, CompiledTemplate compiled);
    const CompiledTemplate* find(const RelPath& source) const;
    size_t size() const { return templates_.size(); }
    // For the compiler, while it builds the app.
    CompiledTemplate* findForUpdate(const RelPath& source);
    FragmentMap& fragmentsForUpdate() { return fragments_; }

    void setFragments(FragmentMap fragments) { fragments_ = std::move(fragments); }
    const CompiledTemplate* fragment(const std::string& name) const;
    const FragmentMap& fragments() const { return fragments_; }

private:
    std::map<RelPath, CompiledTemplate> templates_;
    FragmentMap fragments_;
};

// --- Databases (overview §3, Middle tier, Tier 2, SQL dialects) ---

// A named query: one SQL statement from code/queries/ (or a folder
// migration's own), prepared for the app's database. The only SQL app code
// can run, so SQL injection can't happen: values are always bound.
struct NamedQuery {
    std::string name;   // "findUser", or "admin/users" for queries/admin/users.sql
    RelPath source;     // the file used: "queries/findUser.pg.sql"
    PreparedSql sql;
};

// Named queries by name.
using QuerySet = std::map<std::string, std::shared_ptr<const NamedQuery>>;

// A migration in code/migrations/ (overview §3, Schema changes and
// migrations), as the compiler found it for the app's database.
struct Migration {
    enum class Kind {
        Sql,      // 004_add_index.sql (or its .sqlite.sql or .pg.sql variant)
        Script,   // 005_split_log.ms: data files only, no database
        Folder,   // 003_backfill/: migrate.ms, its own queries and helpers
    };

    int number = 0;
    std::string name;   // "003_backfill_entry_time", without extension
    Kind kind = Kind::Sql;
    // The file applied (the variant for the app's database), the .ms file,
    // or the folder.
    RelPath source;
    // Of what's applied: the file's name and contents, or every file in the
    // folder the database uses. Recorded when applied; a later change is a
    // compile error.
    std::string checksum;
    std::string sql;      // Sql: the statements, run as they are
    QuerySet queries;     // Folder: what its db.query can name

    bool usesDatabase() const { return kind != Kind::Script; }
    // "003_backfill_entry_time.sql", "005_split_log.ms", "003_backfill_entry_time/"
    std::string fileLabel() const;
};

// --- Jobs and push (overview §2, Background jobs; §7) ---

// A job in code/schedule.ms: its name (job.x in code/jobs/) and when.
struct ScheduledJob {
    std::string job;
    Schedule schedule;
};

// One rule from code/access.ms, kept for paths that aren't routes: push
// channels, /live/<name>, whose names aren't known at compile time.
struct AccessRule {
    std::string path;                 // "/live/orders"
    std::vector<std::string> anyOf;   // empty: "login" (unless isPublic)
    bool isPublic = false;            // "public": covers the path, requires nothing
};

// A push channel's name: one or more segments of letters, digits, '-' and
// '_', joined by '/' ("orders", "rooms/lobby"); at most 100 bytes.
bool isValidChannelName(const std::string& name);

// Where the engine serves push channels: /live/<name>.
extern const char* const kLivePrefix;   // "/live/"

struct CompiledApp {
    uint64_t generation = 0;
    RouteTable routes;
    StaticIndex statics;
    TemplateSet templates;
    // frags/layout/markdown.html, which wraps every Markdown page, if the
    // app has one (with {{content}} and {{title}}); else a built-in page.
    std::optional<CompiledTemplate> markdownLayout;
    // frags/auth/layout.html, which wraps the engine's sign-in pages (with
    // {{content}}, the engine's form or message, and {{title}}), if the app
    // has one; else a built-in page.
    std::optional<CompiledTemplate> authLayout;
    // pages/markdown.css exists, so pages with Markdown link it instead of
    // the built-in stylesheet.
    bool hasMarkdownCss = false;
    std::optional<RelPath> notFoundPage;    // pages/404.html, if present
    std::optional<RelPath> errorPage;       // pages/500.html, if present
    std::optional<RelPath> tooManyPage;     // pages/429.html, if present
    // pages/503.html, if present: "down for maintenance", while migrations
    // run longer than requests may wait.
    std::optional<RelPath> unavailablePage;
    // Every form declaration, by FormSpec::key() ("pages/guestbook.ms#sign"),
    // for formState().
    std::map<std::string, std::shared_ptr<const FormSpec>> forms;
    // The database the app's SQL was prepared for (from the config; SQLite
    // unless config.db names Postgres).
    SqlDialect dialect = SqlDialect::Sqlite;
    QuerySet queries;                       // code/queries/
    std::vector<Migration> migrations;      // code/migrations/, by number
    // code/seed.ms (overview §3, Dev mode), if present: run in dev mode after
    // "reset dev data".
    std::optional<RelPath> seed;
    // code/outbound.ms: where `http` may send requests, and `mail` mail.
    OutboundPolicy outbound;
    // Jobs (code/jobs/), by name ("sendWelcome" for job.sendWelcome).
    std::map<std::string, HandlerRef> jobs;
    // code/schedule.ms, by job name.
    std::vector<ScheduledJob> schedule;
    // code/access.ms's rules, for push channels (/live/<name>).
    std::vector<AccessRule> accessRules;
    // Who may subscribe to channel `name` (/live/<name>): every access.ms
    // rule covering its path, combined; nullopt if no rule covers it (the
    // channel isn't served). `sources` gets the rules that applied.
    std::optional<AccessLevel> channelAccess(const std::string& name, std::vector<std::string>* sources = nullptr) const;
    // Mail templates (code/mail/welcome.md), by name ("welcome"): Markdown
    // templates, sent as text (their Markdown) and HTML.
    std::map<std::string, CompiledTemplate> mailTemplates;
    // The app has queries or database migrations, so it uses a database.
    bool usesDatabase() const;
    // Null if the app has no .ms files. Shared by every request on this
    // generation; the bytecode is released when the last one lets go.
    std::shared_ptr<const ScriptBundle> scripts;
    std::vector<Diagnostic> diagnostics;    // warnings that didn't stop the build
};

// A readable listing of the app: every route, then the diagnostics. Used
// for golden tests, and later for the dev tools' route report.
std::string describe(const CompiledApp& app);

}  // namespace fourfold

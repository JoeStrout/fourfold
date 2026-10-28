#include "authflow/auth_flow.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "base/crypto.h"
#include "mail/mailer.h"
#include "pipeline/cookies.h"
#include "template/escape.h"
#include "template/template.h"

namespace fourfold {

namespace {

const char* const kFakeDomain = "fake.invalid";

const char* const kBuiltinStyle =
    "body{margin:0;font:16px/1.5 -apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;background:#f6f6f4;"
    "color:#222}main{max-width:26rem;margin:4rem auto;padding:2rem;background:#fff;border-radius:8px;"
    "box-shadow:0 1px 3px rgba(0,0,0,.12)}h1{font-size:1.4rem;margin-top:0}label{display:block;margin:1rem 0}"
    "input[type=email]{display:block;width:100%;box-sizing:border-box;padding:.5rem;margin-top:.25rem;"
    "font:inherit}button{font:inherit;padding:.5rem 1rem;margin:.25rem .5rem .25rem 0;cursor:pointer}"
    ".message{color:#a33}.note{color:#666;font-size:.9rem}";

std::string esc(const std::string& s) { return escapeHtml(s); }

std::string field(const RequestFacts::Pairs& pairs, const std::string& name) {
    for (const auto& [k, v] : pairs) {
        if (k == name) return v;
    }
    return "";
}

// The opening of a form posting to `action`, with the CSRF token: the
// engine's forms are inside a field, where the template's own injection
// doesn't reach.
std::string formStart(const AuthRequest& r, const std::string& action) {
    return "<form method=\"post\" action=\"" + esc(action) + "\"><input type=\"hidden\" name=\"_csrf\" value=\"" +
           esc(r.render.csrfToken) + "\">";
}

std::string hidden(const std::string& name, const std::string& value) {
    return "<input type=\"hidden\" name=\"" + esc(name) + "\" value=\"" + esc(value) + "\">";
}

std::string localPart(const std::string& email) { return email.substr(0, email.find('@')); }

bool isAdmin(const AppConfig& config, const std::string& email) {
    return std::find(config.auth.admins.begin(), config.auth.admins.end(), email) != config.auth.admins.end();
}

HttpResponse redirect(const std::string& location) {
    HttpResponse r;
    r.status = 303;
    r.headers.set("Location", location);
    r.headers.set("Cache-Control", "no-store");
    return r;
}

}  // namespace

// One request, with what answering it needs.
struct AuthFlow::Context {
    AuthRequest r;
    std::function<void(HttpResponse)> done;

    const AppConfig& config() const { return *r.config; }

    void log(EngineLog* log, const std::string& message) const {
        if (log) log->write("auth", message);
    }

    // A page: the app's sign-in layout if it has one, else a plain one.
    void page(int status, const std::string& title, const std::string& content, EngineLog* engineLog) const {
        HttpResponse response;
        response.status = status;
        response.headers.set("Content-Type", "text/html; charset=utf-8");
        response.headers.set("Cache-Control", "no-store");
        const CompiledTemplate* layout = r.app && r.app->authLayout ? &*r.app->authLayout : nullptr;
        if (layout) {
            MapFields fields;
            fields.set("title", FieldValue::string(title));
            fields.set("content", FieldValue::markup(unsafeHtml(content)));   // the engine's own markup
            RenderOptions options = r.render;
            options.warnings = false;
            RenderResult rendered = render(*layout, fields, options);
            if (rendered.ok()) {
                response.body = rendered.html.str();
                done(std::move(response));
                return;
            }
            log(engineLog, "frags/auth/layout.html couldn't be rendered: " + rendered.error->message);
        }
        response.body = "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" +
                        esc(title) + "</title><style>" + kBuiltinStyle + "</style></head>\n<body><main>\n" + content +
                        "\n</main></body></html>\n";
        done(std::move(response));
    }

    // Where links in mail point: the request's own origin in dev mode, else
    // the configured domain.
    std::string base() const {
        if (config().mode == ConfigMode::Dev || config().site.domain.empty()) return r.origin;
        return std::string(r.https ? "https://" : "http://") + config().site.domain;
    }
};

AuthFlow::AuthFlow(AuthStore& auth, SessionStore& sessions, LockService& sessionLocks, WorkerPool& workers,
                   EngineLog* log)
    : auth_(auth), sessions_(sessions), locks_(sessionLocks), workers_(workers), log_(log) {}

std::string AuthFlow::safeNext(const std::string& next) {
    if (next.empty() || next[0] != '/' || next.size() > 2000) return "/";
    if (next.size() > 1 && (next[1] == '/' || next[1] == '\\')) return "/";
    for (unsigned char c : next) {
        if (c < 0x20 || c == 0x7F || c == '\\') return "/";
    }
    if (next == "/__auth" || next.compare(0, 8, "/__auth/") == 0) return "/";
    return next;
}

std::vector<FakeUser> AuthFlow::fakeUsers(const CompiledApp* app) {
    std::vector<FakeUser> users = {{"admin", "Admin (fake)", {"admin"}}, {"member", "Member (fake)", {}}};
    std::set<std::string> roles;
    if (app) {
        for (const auto& [url, entry] : app->routes) {
            for (const auto& anyOf : entry.access.all) roles.insert(anyOf.begin(), anyOf.end());
        }
    }
    roles.erase("admin");
    for (const std::string& role : roles) {
        if (role == "member") continue;   // the plain member's key
        std::string name = role;
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        users.push_back({role, name + " (fake)", {role}});
    }
    return users;
}

void AuthFlow::handle(AuthRequest request, std::function<void(HttpResponse)> done) {
    auto c = std::make_shared<Context>(Context{std::move(request), std::move(done)});
    // Everything here may touch SQLite or the network: on a worker.
    workers_.submit([this, c] {
        const std::string& path = c->r.path;
        bool post = c->r.method == "POST";
        bool get = c->r.method == "GET" || c->r.method == "HEAD";
        if (path == "/__auth/login" && get) return loginPage(*c);
        if (path == "/__auth/login" && post) return requestLink(*c);
        if (path == "/__auth/link" && get) return confirmPage(*c);
        if (path == "/__auth/link" && post) return redeem(*c);
        if (path == "/__auth/logout" && get) return logoutPage(*c);
        if (path == "/__auth/logout" && post) return signOut(*c);
        if (path == "/__auth/fake" && c->config().auth.fakeLogin && get) return fakePage(*c);
        if (path == "/__auth/fake" && c->config().auth.fakeLogin && post) return fakeSignIn(*c);
        c->done(HttpResponse::text(404, "Not Found\n"));
    });
}

void AuthFlow::loginPage(const Context& c, const std::string& message, int status) {
    const AuthRequest& r = c.r;
    std::string next = safeNext(field(r.form, "next").empty() ? field(r.query, "next") : field(r.form, "next"));
    std::string content = "<h1>Sign in</h1>\n";
    if (r.user) {
        content += "<p>You're signed in as " + esc(r.user->name) + " (" + esc(r.user->email) +
                   ").</p>\n<p><a href=\"" + esc(next) + "\">Continue</a></p>\n" + formStart(r, "/__auth/logout") +
                   hidden("next", "/") + "<button type=\"submit\">Sign out</button></form>\n";
        return c.page(200, "Sign in", content, log_);
    }
    if (!message.empty()) content += "<p class=\"message\">" + esc(message) + "</p>\n";
    bool email = c.config().auth.magicLink && c.config().mailer;
    if (email) {
        content += formStart(r, "/__auth/login") + hidden("next", next) +
                   "<label>Your email address <input type=\"email\" name=\"email\" required autocomplete=\"email\" "
                   "value=\"" + esc(field(r.form, "email")) + "\"></label>\n"
                   "<button type=\"submit\">Email me a sign-in link</button></form>\n"
                   "<p class=\"note\">We'll email you a link. No password needed.</p>\n";
    } else {
        content += "<p>Signing in by email isn't set up for this site yet.</p>\n";
    }
    if (c.config().auth.fakeLogin) {
        content += "<p class=\"note\">Dev mode: <a href=\"/__auth/fake?next=" + esc(next) +
                   "\">sign in as a test user</a>.</p>\n";
    }
    c.page(status, "Sign in", content, log_);
}

void AuthFlow::requestLink(const Context& c) {
    const AuthRequest& r = c.r;
    const AppConfig& config = c.config();
    if (!config.auth.magicLink || !config.mailer) return loginPage(c, "", 404);
    std::string email = AuthStore::normalizeEmail(field(r.form, "email"));
    if (!isValidEmailAddress(email)) return loginPage(c, "That doesn't look like an email address.", 400);
    std::string next = safeNext(field(r.form, "next"));

    std::string sent = "<h1>Check your email</h1>\n<p>If <strong>" + esc(email) +
                       "</strong> can sign in here, a sign-in link is on its way. It works once, for " +
                       std::to_string(config.auth.store.tokenLifetime.count() / 60'000) +
                       " minutes.</p>\n<p class=\"note\">Nothing arrived? Check your spam folder, or <a href=\"/__auth/login?next=" +
                       esc(next) + "\">try again</a>.</p>\n";

    auto user = auth_.findByEmail(email);
    if (!user) {
        c.log(log_, "looking up a user failed: " + user.error().message);
        return c.page(500, "Sign in", "<h1>Sign in</h1>\n<p>Something went wrong. Please try again.</p>", log_);
    }
    if (!user.value() && !config.auth.openSignUp && !isAdmin(config, email)) {
        // The same answer as for an address that may sign in.
        c.log(log_, "no sign-in link for " + email + ": sign-up is closed, and there's no such user");
        return c.page(200, "Check your email", sent, log_);
    }
    auto token = auth_.createToken(email, r.addressGroup, next);
    if (!token) {
        c.log(log_, "making a sign-in link failed: " + token.error().message);
        return c.page(500, "Sign in", "<h1>Sign in</h1>\n<p>Something went wrong. Please try again.</p>", log_);
    }
    if (!token.value()) {
        c.log(log_, "sign-in links throttled for " + email + " from " + r.addressGroup);
        return c.page(429, "Sign in",
                      "<h1>Too many sign-in links</h1>\n<p>Too many links have been asked for. Please wait a while, "
                      "then try again.</p>",
                      log_);
    }
    std::string link = c.base() + "/__auth/link?token=" + *token.value();
    MailMessage message;
    message.from = config.mail.from;
    message.to = email;
    message.subject = "Your sign-in link";
    message.text = "Hello,\n\nTo sign in, open this link:\n\n" + link + "\n\nIt works once, for " +
                   std::to_string(config.auth.store.tokenLifetime.count() / 60'000) +
                   " minutes.\n\nIf you didn't ask to sign in, you can ignore this email.\n";
    if (Status s = config.mailer->send(message); !s) {
        c.log(log_, "sending a sign-in link to " + email + " failed: " + s.error().message);
        return c.page(502, "Sign in",
                      "<h1>Sign in</h1>\n<p>The sign-in email couldn't be sent. Please try again later.</p>", log_);
    }
    c.log(log_, "sign-in link sent to " + email);
    c.page(200, "Check your email", sent, log_);
}

void AuthFlow::confirmPage(const Context& c) {
    const AuthRequest& r = c.r;
    std::string token = field(r.query, "token");
    c.page(200, "Sign in",
           "<h1>Sign in</h1>\n" + formStart(r, "/__auth/link") + hidden("token", token) +
               "<button type=\"submit\">Sign in</button></form>\n",
           log_);
}

void AuthFlow::redeem(const Context& c) {
    const AuthRequest& r = c.r;
    const AppConfig& config = c.config();
    auto used = auth_.redeemToken(field(r.form, "token"));
    if (!used) {
        c.log(log_, "redeeming a sign-in link failed: " + used.error().message);
        return c.page(500, "Sign in", "<h1>Sign in</h1>\n<p>Something went wrong. Please try again.</p>", log_);
    }
    if (!used.value()) {
        c.log(log_, "a sign-in link was used again, or late, or was never issued (from " + r.addressGroup + ")");
        return c.page(400, "Sign in",
                      "<h1>This link has expired</h1>\n<p>Sign-in links work once, for a short time. "
                      "<a href=\"/__auth/login\">Get a new one</a>.</p>",
                      log_);
    }
    const std::string& email = used.value()->email;
    auto user = auth_.findByEmail(email);
    std::optional<AuthUser> u = user ? user.value() : std::nullopt;
    if (user && !u) {
        if (!config.auth.openSignUp && !isAdmin(config, email)) {
            c.log(log_, "sign-in refused for " + email + ": sign-up is closed");
            return c.page(403, "Sign in", "<h1>Sign in</h1>\n<p>This address can't sign in here.</p>", log_);
        }
        auto created = auth_.create(email, localPart(email));
        if (created) {
            u = created.value();
            c.log(log_, "new user " + u->id + " (" + email + ")");
        } else if (created.error().code == ErrorCode::Conflict) {
            auto again = auth_.findByEmail(email);   // made meanwhile by another sign-in
            if (again) u = again.value();
        }
    }
    if (!u) {
        c.log(log_, "finding or making the user for " + email + " failed");
        return c.page(500, "Sign in", "<h1>Sign in</h1>\n<p>Something went wrong. Please try again.</p>", log_);
    }
    if (isAdmin(config, email) && std::find(u->roles.begin(), u->roles.end(), "admin") == u->roles.end()) {
        if (auth_.grant(u->id, "admin")) {
            u->roles.push_back("admin");
            c.log(log_, email + " is an admin (config.auth.admins)");
        }
    }
    signIn(c, *u, used.value()->next);
}

void AuthFlow::signIn(const Context& c, const AuthUser& user, const std::string& next) {
    auto shared = std::make_shared<Context>(c);
    std::string target = safeNext(next);
    // Rotation, under the old session's lock, so a request using it can't
    // save over it meanwhile.
    auto rotate = [this, shared, user, target](std::optional<LockService::Ticket> ticket) {
        const AuthRequest& r = shared->r;
        std::string app = "{}";
        if (r.sessionHash) {
            if (auto old = sessions_.load(*r.sessionHash); old && old.value()) {
                // An anonymous session's map carries over; another user's doesn't.
                if (!old.value()->userId || *old.value()->userId == user.id) app = old.value()->app;
            }
        }
        std::string id = SessionStore::newId();
        Status s = sessions_.signIn(r.sessionHash, id, user.id, r.addressGroup, app);
        if (ticket) locks_.release(*ticket);
        if (!s) {
            shared->log(log_, "signing in " + user.email + " failed: " + s.error().message);
            return shared->page(500, "Sign in", "<h1>Sign in</h1>\n<p>Something went wrong. Please try again.</p>",
                                log_);
        }
        shared->log(log_, "signed in: " + user.email + " (" + user.id + ") from " + r.addressGroup);
        HttpResponse response = redirect(target);
        int64_t maxAge =
            std::chrono::duration_cast<std::chrono::seconds>(sessions_.config().signedInLimit).count();
        response.headers.add("Set-Cookie", setCookie("session", id, r.https, maxAge));
        shared->done(std::move(response));
    };
    if (!c.r.sessionHash) return rotate(std::nullopt);
    std::string key = "session:" + toHex(*c.r.sessionHash);
    locks_.acquire(key, [this, rotate](Status locked, LockService::Ticket ticket) {
        workers_.submit([rotate, locked, ticket] {
            (void)locked;   // without the lock, rotate anyway: the old session goes either way
            rotate(ticket);
        });
    });
}

void AuthFlow::logoutPage(const Context& c) {
    const AuthRequest& r = c.r;
    if (!r.user) {
        return c.page(200, "Sign out", "<h1>Sign out</h1>\n<p>You're not signed in. <a href=\"/\">Home</a></p>", log_);
    }
    c.page(200, "Sign out",
           "<h1>Sign out</h1>\n<p>Signed in as " + esc(r.user->name) + " (" + esc(r.user->email) + ").</p>\n" +
               formStart(r, "/__auth/logout") + "<button type=\"submit\">Sign out</button></form>\n" +
               formStart(r, "/__auth/logout") + hidden("everywhere", "1") +
               "<button type=\"submit\">Sign out on every device</button></form>\n",
           log_);
}

void AuthFlow::signOut(const Context& c) {
    const AuthRequest& r = c.r;
    Status s;
    if (field(r.form, "everywhere") == "1" && r.user) {
        s = sessions_.removeForUser(r.user->id);
        if (s) c.log(log_, "signed out everywhere: " + r.user->email);
    } else if (r.sessionHash) {
        s = sessions_.remove(*r.sessionHash);
        if (s && r.user) c.log(log_, "signed out: " + r.user->email);
    }
    if (!s) c.log(log_, "signing out failed: " + s.error().message);
    HttpResponse response = redirect(safeNext(field(r.form, "next")));
    response.headers.add("Set-Cookie", setCookie("session", "", r.https, 0));
    c.done(std::move(response));
}

void AuthFlow::fakePage(const Context& c) {
    const AuthRequest& r = c.r;
    std::string next = safeNext(field(r.query, "next"));
    std::string content = "<h1>Sign in as&hellip;</h1>\n<p class=\"note\">Dev mode's test users. Each has its own "
                          "session.</p>\n";
    if (r.user) content += "<p>Now: " + esc(r.user->name) + " (" + esc(r.user->email) + ")</p>\n";
    for (const FakeUser& u : fakeUsers(r.app.get())) {
        std::string roles;
        for (const std::string& role : u.roles) roles += (roles.empty() ? "" : ", ") + role;
        content += formStart(r, "/__auth/fake") + hidden("who", u.key) + hidden("next", next) +
                   "<button type=\"submit\">" + esc(u.name) + "</button> <span class=\"note\">" +
                   (roles.empty() ? std::string("no roles") : esc(roles)) + "</span></form>\n";
    }
    if (r.user) {
        content += formStart(r, "/__auth/logout") + hidden("next", next) +
                   "<button type=\"submit\">Sign out</button></form>\n";
    }
    c.page(200, "Sign in as…", content, log_);
}

void AuthFlow::fakeSignIn(const Context& c) {
    const AuthRequest& r = c.r;
    std::string who = field(r.form, "who");
    for (const FakeUser& f : fakeUsers(r.app.get())) {
        if (f.key != who) continue;
        std::string email = f.key + "@" + kFakeDomain;
        auto found = auth_.findByEmail(email);
        std::optional<AuthUser> u = found ? found.value() : std::nullopt;
        if (found && !u) {
            auto created = auth_.create(email, f.name);
            if (created) u = created.value();
        }
        if (!u) return c.page(500, "Sign in", "<h1>Sign in</h1>\n<p>The test user couldn't be made.</p>", log_);
        for (const std::string& role : f.roles) {
            if (std::find(u->roles.begin(), u->roles.end(), role) == u->roles.end() && auth_.grant(u->id, role)) {
                u->roles.push_back(role);
            }
        }
        return signIn(c, *u, field(r.form, "next"));
    }
    c.page(400, "Sign in", "<h1>Sign in</h1>\n<p>No such test user.</p>", log_);
}

}  // namespace fourfold

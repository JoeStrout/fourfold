// End to end (M6): access rules, sign-in and sign-out through `fourfold
// dev`'s server, on a real socket. The clubhouse sample app is requested
// route by route as each kind of visitor; a small app signs in by magic link
// through an SMTP server in this process.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>

#include "app_server.h"
#include "base/datum.h"
#include "browser.h"
#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::test::bodyOf;
using fourfold::test::Browser;
using fourfold::test::rawFetch;
using fourfold::test::TempDir;
using fourfold::test::textBetween;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int statusOf(const std::string& response) {
    return response.size() > 12 ? std::atoi(response.c_str() + 9) : 0;
}

std::string headerOf(const std::string& response, const std::string& name) {
    std::string head = response.substr(0, response.find("\r\n\r\n"));
    return textBetween(head, "\r\n" + name + ": ", "\r\n");
}

// The clubhouse sample, copied out of the source tree so the test's data
// and state stay out of it.
std::string copyClubhouse(const TempDir& dir) {
    namespace fs = std::filesystem;
    fs::path from = fs::path(FOURFOLD_TESTS_DIR) / ".." / "examples" / "clubhouse";
    fs::path to = dir.at("clubhouse");
    fs::create_directories(to);
    for (const char* part : {"code", "private/config"}) {
        fs::create_directories(to / part);
        fs::copy(from / part, to / part, fs::copy_options::recursive);
    }
    return to.string();
}

// Signs a browser in as one of the fake login's test users.
void fakeSignIn(Browser& b, const std::string& who) {
    REQUIRE_EQ(statusOf(b.get("/__auth/fake")), 200);
    std::string r = b.post("/__auth/fake", "who=" + who + "&next=/");
    REQUIRE_MESSAGE(statusOf(r) == 303, r);
}

// Whether an access level, as the report writes it ("public", "login",
// "admin or moderator", "login and editor"), lets in a visitor with these
// roles (nullptr: not signed in).
bool permits(const std::string& access, const std::vector<std::string>* roles) {
    if (access == "public") return true;
    if (!roles) return false;
    std::istringstream words(access);
    std::string word;
    bool met = false, any = false;
    auto settle = [&] {
        bool ok = !any || met;
        met = false;
        any = false;
        return ok;
    };
    while (words >> word) {
        word.erase(std::remove(word.begin(), word.end(), '('), word.end());
        word.erase(std::remove(word.begin(), word.end(), ')'), word.end());
        if (word == "or") continue;
        if (word == "and") {
            if (!settle()) return false;
            continue;
        }
        if (word == "login") continue;
        any = true;
        met = met || std::find(roles->begin(), roles->end(), word) != roles->end();
    }
    return settle();
}

}  // namespace

TEST_CASE("access: every route of the clubhouse, as each kind of visitor") {
    TempDir dir;
    std::string root = copyClubhouse(dir);
    std::ostringstream out, err;
    LifecycleOptions lifecycle;
    lifecycle.trigger = LifecycleOptions::Trigger::DeployOnly;
    auto server = AppServer::startDev(root, 0, out, err, lifecycle);
    REQUIRE_MESSAGE(server, err.str());
    REQUIRE_MESSAGE(server->compiled(), err.str());
    std::thread loop([&] { server->loop().run(); });
    uint16_t port = server->port();

    Browser anon(port), member(port), moderator(port), admin(port);
    (void)anon.get("/");   // a CSRF token, so its posts fail on access alone
    fakeSignIn(member, "member");
    fakeSignIn(moderator, "moderator");
    fakeSignIn(admin, "admin");
    const std::vector<std::string> memberRoles, moderatorRoles{"moderator"}, adminRoles{"admin"};
    struct Who {
        const char* name;
        Browser* browser;
        const std::vector<std::string>* roles;
    };
    const Who visitors[] = {{"anonymous", &anon, nullptr},
                            {"member", &member, &memberRoles},
                            {"moderator", &moderator, &moderatorRoles},
                            {"admin", &admin, &adminRoles}};

    // The access report: every route, with its level.
    std::string reportResponse = admin.get("/__dev/access.json");
    REQUIRE_EQ(statusOf(reportResponse), 200);
    auto report = parseJson(bodyOf(reportResponse));
    REQUIRE(report.ok());
    std::map<std::string, std::string> levels;
    for (const Datum& r : report.value().asMap().at("routes").asList()) {
        levels[r.asMap().at("url").asString()] = r.asMap().at("access").asString();
    }
    const std::map<std::string, std::string> expectedLevels = {
        {"/", "public"},           {"/about", "public"},
        {"/admin/", "admin"},      {"/admin/grant", "admin"},
        {"/admin/revoke", "admin"}, {"/board", "public"},
        {"/board/post", "login"},  {"/members/", "login"},
        {"/members/badge.svg", "login"}, {"/members/handbook", "login"},
        {"/mod/", "admin or moderator"}, {"/style.css", "public"},
    };
    CHECK_EQ(levels, expectedLevels);
    CHECK(report.value().asMap().at("warnings").asList().empty());
    CHECK(contains(bodyOf(admin.get("/__dev/access")), "<td>admin or moderator</td>"));

    // Every route, as each visitor: GETs for pages and files, POSTs for
    // handlers. Expected: anonymous visitors are sent to sign in (GET) or
    // refused with 401 (POST); signed-in visitors without the role get 403.
    struct Request {
        const char* method;
        const char* url;
        const char* body;
        int okStatus;   // what an allowed visitor gets
    };
    const Request requests[] = {
        {"GET", "/", "", 200},
        {"GET", "/about", "", 200},
        {"GET", "/style.css", "", 200},
        {"GET", "/board", "", 200},
        {"GET", "/members/", "", 200},
        {"GET", "/members/handbook", "", 200},
        {"GET", "/members/badge.svg", "", 200},
        {"GET", "/mod/", "", 200},
        {"GET", "/admin/", "", 200},
        {"POST", "/board/post", "text=hello", 303},
        {"POST", "/admin/grant", "email=nobody%40example.com", 303},
        {"POST", "/admin/revoke", "email=nobody%40example.com", 303},
    };
    for (const Request& req : requests) {
        const std::string& level = levels.at(req.url);
        for (const Who& who : visitors) {
            INFO(req.method << " " << req.url << " as " << who.name << " (" << level << ")");
            std::string r = who.browser->send(req.method, req.url, req.body);
            int status = statusOf(r);
            bool allowed = permits(level, who.roles);
            if (allowed) {
                CHECK_EQ(status, req.okStatus);
            } else if (!who.roles) {
                if (std::string(req.method) == "GET") {
                    CHECK_EQ(status, 303);
                    CHECK_EQ(headerOf(r, "Location"), "/__auth/login?next=" + std::string(req.url));
                } else {
                    CHECK_EQ(status, 401);
                }
            } else {
                CHECK_EQ(status, 403);
            }
            if (level != "public") {
                CHECK_EQ(headerOf(r, "Cache-Control"), "private, no-store");
            }
            if (!allowed) CHECK_FALSE(contains(r, "Members only"));
        }
    }

    // An HTMX request is told where to sign in, instead of following a redirect.
    std::string htmx = anon.send("GET", "/members/", "", true);
    CHECK_EQ(statusOf(htmx), 401);
    CHECK_EQ(headerOf(htmx, "HX-Redirect"), "/__auth/login?next=/members/");

    // req.user reaches handlers: the board's post is signed with the name.
    CHECK(contains(bodyOf(anon.get("/board")), "<b>Member (fake)</b>: hello"));

    // Roles are read on every request: making the member a moderator lets
    // them in at once, and taking it back shuts them out again.
    CHECK_EQ(statusOf(member.get("/mod/")), 403);
    admin.get("/admin/");
    CHECK_EQ(statusOf(admin.post("/admin/grant", "email=member%40fake.invalid")), 303);
    CHECK(contains(bodyOf(admin.get("/admin/")), "is a moderator now"));
    CHECK_EQ(statusOf(member.get("/mod/")), 200);
    CHECK_EQ(statusOf(admin.post("/admin/revoke", "email=member%40fake.invalid")), 303);
    CHECK_EQ(statusOf(member.get("/mod/")), 403);

    server->loop().stop();
    loop.join();
}

namespace {

// An SMTP server on 127.0.0.1 that keeps every message it's sent.
class FakeSmtp {
public:
    FakeSmtp() {
        listener_ = Fd(socket(AF_INET, SOCK_STREAM, 0));
        int one = 1;
        setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        listen(listener_.get(), 8);
        socklen_t len = sizeof addr;
        getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeSmtp() {
        stop_ = true;
        thread_.join();
    }

    int port() const { return port_; }

    std::vector<std::string> messages() {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }

private:
    void serve() {
        while (!stop_) {
            pollfd p{listener_.get(), POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            Fd c(accept(listener_.get(), nullptr, nullptr));
            if (!c.valid()) continue;
            session(c.get());
        }
    }

    static void sendAll(int fd, const std::string& s) { (void)!::send(fd, s.data(), s.size(), 0); }

    static std::string readUntil(int fd, std::string& buffer, const std::string& end) {
        for (;;) {
            size_t at = buffer.find(end);
            if (at != std::string::npos) {
                std::string out = buffer.substr(0, at + end.size());
                buffer.erase(0, at + end.size());
                return out;
            }
            char chunk[4096];
            ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
            if (n <= 0) return "";
            buffer.append(chunk, static_cast<size_t>(n));
        }
    }

    void session(int fd) {
        std::string buffer, message;
        sendAll(fd, "220 fake ESMTP\r\n");
        for (;;) {
            std::string line = readUntil(fd, buffer, "\r\n");
            if (line.empty()) return;
            std::string verb = line.substr(0, 4);
            if (verb == "EHLO") {
                sendAll(fd, "250-fake\r\n250 8BITMIME\r\n");
            } else if (verb == "DATA") {
                sendAll(fd, "354 go ahead\r\n");
                message = readUntil(fd, buffer, "\r\n.\r\n");
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    messages_.push_back(message);
                }
                sendAll(fd, "250 queued\r\n");
            } else if (verb == "QUIT") {
                sendAll(fd, "221 bye\r\n");
                return;
            } else {
                sendAll(fd, "250 ok\r\n");
            }
        }
    }

    Fd listener_;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> messages_;
};

// A quoted-printable body, decoded.
std::string decodeQp(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '=' && i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') {
            i += 2;   // a soft line break
        } else if (s[i] == '=' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The sign-in link's path in a message ("/__auth/link?token=...").
std::string linkIn(const std::string& message) {
    std::string body = decodeQp(message.substr(message.find("\r\n\r\n") + 4));
    size_t at = body.find("/__auth/link?token=");
    REQUIRE(at != std::string::npos);
    size_t end = body.find_first_of("\r\n", at);
    return body.substr(at, end - at);
}

void writeSignInApp(const TempDir& dir, int smtpPort, bool openSignUp) {
    dir.write("app/code/pages/whoami.ms",
              "render = function(req)\n"
              "    who = \"anonymous\"\n"
              "    if req.user != null then who = req.user.email + \" [\" + req.user.roles.join(\",\") + \"]\"\n"
              "    cart = 0\n"
              "    if session.hasIndex(\"cart\") then cart = session.cart.len\n"
              "    return who + \" cart=\" + cart\n"
              "end function\n");
    dir.write("app/code/pages/cart.ms",
              "get.add = function(req)\n"
              "    if not session.hasIndex(\"cart\") then session.cart = []\n"
              "    session.cart.push 1\n"
              "    return \"added\"\n"
              "end function\n");
    dir.write("app/private/config/app.ms",
              "config = {}\n"
              "config.auth = {\"admins\": [\"boss@example.com\"], \"signUp\": \"" +
                  std::string(openSignUp ? "open" : "closed") +
                  "\", \"fakeLogin\": false}\n"
                  "config.mail = {\"transport\": \"smtp\", \"from\": \"App <noreply@example.com>\",\n"
                  "  \"smtpHost\": \"127.0.0.1\", \"smtpPort\": " +
                  std::to_string(smtpPort) + ", \"smtpSecurity\": \"none\"}\n");
}

// Asks for a link for `email`; returns the link's path from the mail sent.
std::string requestLink(Browser& b, FakeSmtp& smtp, const std::string& email) {
    size_t before = smtp.messages().size();
    REQUIRE_EQ(statusOf(b.get("/__auth/login?next=/whoami")), 200);
    std::string r = b.post("/__auth/login", "email=" + email + "&next=/whoami");
    REQUIRE_MESSAGE(statusOf(r) == 200, r);
    CHECK(contains(r, "Check your email"));
    auto messages = smtp.messages();
    REQUIRE_EQ(messages.size(), before + 1);
    return linkIn(messages.back());
}

// Follows a link: the confirmation page, then its POST.
std::string followLink(Browser& b, const std::string& link) {
    std::string page = b.get(link);
    REQUIRE_EQ(statusOf(page), 200);
    std::string token = textBetween(page, "name=\"token\" value=\"", "\"");
    return b.post("/__auth/link", "token=" + token);
}

}  // namespace

TEST_CASE("access: magic-link sign-in, end to end, through an SMTP server") {
    FakeSmtp smtp;
    TempDir dir;
    writeSignInApp(dir, smtp.port(), true);
    std::ostringstream out, err;
    LifecycleOptions lifecycle;
    lifecycle.pollInterval = std::chrono::milliseconds(50);
    auto server = AppServer::startDev(dir.at("app"), 0, out, err, lifecycle);
    REQUIRE_MESSAGE(server, err.str());
    REQUIRE_MESSAGE(server->compiled(), err.str());
    std::thread loop([&] { server->loop().run(); });
    uint16_t port = server->port();

    // An anonymous session, with something in it.
    Browser b(port);
    CHECK_EQ(bodyOf(b.get("/cart/add")), "added");
    std::string anonymousId = b.cookies().at("ff_session");
    CHECK_EQ(bodyOf(b.get("/whoami")), "anonymous cart=1");

    // The link arrives by mail; the GET only shows a button (a mail scanner
    // fetching it signs no one in); the POST signs in.
    std::string link = requestLink(b, smtp, "Ann%40Example.com");
    std::string mail = smtp.messages().back();
    CHECK(contains(mail, "To: <ann@example.com>"));
    CHECK(contains(mail, "Subject: Your sign-in link"));
    CHECK_EQ(bodyOf(b.get("/whoami")), "anonymous cart=1");
    std::string signedIn = followLink(b, link);
    REQUIRE_MESSAGE(statusOf(signedIn) == 303, signedIn);
    CHECK_EQ(headerOf(signedIn, "Location"), "/whoami");
    // The session ID rotated, and the cart came along.
    CHECK_NE(b.cookies().at("ff_session"), anonymousId);
    CHECK_EQ(bodyOf(b.get("/whoami")), "ann@example.com [] cart=1");
    auto ann = server->users().findByEmail("ann@example.com");
    REQUIRE(ann.ok());
    REQUIRE(ann.value());
    CHECK_EQ(ann.value()->name, "ann");

    // The old ID is dead: replaying it gets an anonymous visitor.
    Browser replay(port);
    replay.setCookie("ff_session", anonymousId);
    CHECK_EQ(bodyOf(replay.get("/whoami")), "anonymous cart=0");

    // A link works once.
    Browser again(port);
    again.get("/__auth/login");
    std::string token = textBetween(link, "token=", "&");
    std::string reused = again.post("/__auth/link", "token=" + token);
    CHECK_EQ(statusOf(reused), 400);
    CHECK(contains(reused, "expired"));

    // A configured admin gets the role at sign-in.
    Browser boss(port);
    REQUIRE_EQ(statusOf(followLink(boss, requestLink(boss, smtp, "boss%40example.com"))), 303);
    CHECK_EQ(bodyOf(boss.get("/whoami")), "boss@example.com [admin] cart=0");

    // Sign out deletes the session; sign out everywhere ends the others too.
    Browser second(port);
    REQUIRE_EQ(statusOf(followLink(second, requestLink(second, smtp, "ann%40example.com"))), 303);
    CHECK(contains(bodyOf(second.get("/whoami")), "ann@example.com"));
    REQUIRE_EQ(statusOf(b.get("/__auth/logout")), 200);
    std::string signedOut = b.post("/__auth/logout", "everywhere=1");
    CHECK_EQ(statusOf(signedOut), 303);
    CHECK_FALSE(b.cookies().count("ff_session"));
    CHECK_EQ(bodyOf(b.get("/whoami")), "anonymous cart=0");
    CHECK_EQ(bodyOf(second.get("/whoami")), "anonymous cart=0");
    CHECK(contains(bodyOf(boss.get("/whoami")), "boss@example.com"));   // another user: still in

    // Links are throttled: three an hour for one address (Ann has had two).
    Browser flood(port);
    (void)flood.get("/__auth/login");
    CHECK_EQ(statusOf(flood.post("/__auth/login", "email=ann%40example.com&next=/")), 200);
    std::string throttled = flood.post("/__auth/login", "email=ann%40example.com&next=/");
    CHECK_EQ(statusOf(throttled), 429);

    // A form posted from another site can't sign anyone out (or in).
    std::string forged = rawFetch(port, "POST /__auth/logout HTTP/1.1\r\nHost: localhost\r\nOrigin: https://evil.example"
                                        "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 0\r\n"
                                        "Connection: close\r\n\r\n");
    CHECK_EQ(statusOf(forged), 403);

    // The config is reloaded when it changes: with sign-up closed, an
    // unknown address gets the same answer, and no mail.
    writeSignInApp(dir, smtp.port(), false);
    for (int i = 0; i < 100 && server->config()->auth.openSignUp; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE_FALSE(server->config()->auth.openSignUp);
    size_t sent = smtp.messages().size();
    Browser stranger(port);
    stranger.get("/__auth/login");
    std::string closed = stranger.post("/__auth/login", "email=stranger%40example.com&next=/");
    CHECK_EQ(statusOf(closed), 200);
    CHECK(contains(closed, "Check your email"));
    CHECK_EQ(smtp.messages().size(), sent);
    CHECK(contains(out.str(), "Config reloaded."));

    server->loop().stop();
    loop.join();
}

TEST_CASE("access: serve mode refuses an invalid config, and fake login") {
    TempDir dir;
    dir.write("app/code/pages/index.html", "<!DOCTYPE html><p>hi</p>");
    dir.write("app/private/config/app.ms", "config = {\"auth\": {\"fakeLogin\": true}}\n");
    std::ostringstream out, err;
    cli::ServerOptions options;
    options.dev = false;
    auto server = AppServer::start(dir.at("app"), options, out, err);
    CHECK_FALSE(server);
    CHECK(contains(err.str(), "fakeLogin can't be on"));

    // No host name, no domain, and not on loopback: refused.
    dir.write("app/private/config/app.ms", "config = {}\n");
    options.address = "0.0.0.0";
    std::ostringstream err2;
    CHECK_FALSE(AppServer::start(dir.at("app"), options, out, err2));
    CHECK(contains(err2.str(), "give the site's host name"));
}

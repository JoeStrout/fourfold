// End to end (M9.1): background jobs and push channels, through `fourfold
// dev`'s server on a real socket.
//
// The exit criteria (milestones.md, M9.1) tested here: a job killed mid-run
// is retried; enqueues from a failed request are dropped; a unique key
// enqueues once; missed runs run once after downtime; a feed-polling job
// publishes to open pages over SSE; a channel under a members-only rule
// refuses a visitor who isn't signed in. (Schedules across a DST change are
// tested in tests/compiledapp/schedule_test.cpp, with the zone's files.)
#include <poll.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "app_server.h"
#include "browser.h"
#include "doctest.h"
#include "fake_http.h"
#include "state/job_queue.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::cli::AppServer;
using fourfold::cli::ServerOptions;
using fourfold::test::Browser;
using fourfold::test::FakeHttp;
using fourfold::test::TempDir;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int statusOf(const std::string& response) {
    return response.size() > 12 ? std::atoi(response.c_str() + 9) : 0;
}

struct Server {
    std::ostringstream out, err;
    std::unique_ptr<AppServer> server;
    std::thread serving;

    explicit Server(const std::string& root, std::chrono::milliseconds retry = std::chrono::milliseconds(100)) {
        ServerOptions options;
        options.jobRetryDelay = retry;
        server = AppServer::start(root, options, out, err);
        REQUIRE_MESSAGE(server, err.str());
        REQUIRE_MESSAGE(server->compiled(), (err.str() + out.str()));
        serving = std::thread([this] { server->loop().run(); });
    }
    ~Server() { stop(); }
    void stop() {
        if (!server) return;
        server->loop().stop();
        serving.join();
        server.reset();
    }
    uint16_t port() const { return server->port(); }
};

// Waits (up to `seconds`) for `ready` to be true.
template <typename F>
bool eventually(F ready, double seconds = 10) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
    while (std::chrono::steady_clock::now() < until) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ready();
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A subscriber to a push channel: an open connection, read on a thread.
class Subscriber {
public:
    Subscriber(uint16_t port, const std::string& path, const std::string& cookies = "") {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int connected = connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        for (int tries = 0; connected < 0 && errno == EPERM && tries < 5; tries++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            connected = connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        }
        std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n";
        if (!cookies.empty()) request += "Cookie: " + cookies + "\r\n";
        request += "\r\n";
        (void)::send(fd_, request.data(), request.size(), 0);
        reader_ = std::thread([this] {
            char buffer[4096];
            while (!stop_) {
                pollfd p{fd_, POLLIN, 0};
                if (::poll(&p, 1, 50) <= 0) continue;
                ssize_t n = ::recv(fd_, buffer, sizeof buffer, 0);
                if (n <= 0) {
                    closed_ = true;
                    break;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                received_.append(buffer, static_cast<size_t>(n));
            }
        });
    }
    ~Subscriber() {
        stop_ = true;
        reader_.join();
        ::close(fd_);
    }
    std::string received() {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }
    bool waitFor(const std::string& text, double seconds = 10) {
        return eventually([&] { return contains(received(), text); }, seconds);
    }
    bool closed() const { return closed_; }

private:
    int fd_ = -1;
    std::thread reader_;
    std::atomic<bool> stop_{false}, closed_{false};
    std::mutex mutex_;
    std::string received_;
};

std::string cookieHeader(const Browser& b) {
    std::string out;
    for (const auto& [name, value] : b.cookies()) out += (out.empty() ? "" : "; ") + name + "=" + value;
    return out;
}

void fakeSignIn(Browser& b, const std::string& who) {
    REQUIRE_EQ(statusOf(b.get("/__auth/fake")), 200);
    std::string r = b.post("/__auth/fake", "who=" + who + "&next=/");
    REQUIRE_MESSAGE(statusOf(r) == 303, r);
}

const char* const kJobs =
    "job.record = function(args)\n"
    "    data.append \"log\", args.what\n"
    "    if args.hasIndex(\"next\") then enqueue args.next, {\"what\": \"chained\"}\n"
    "    return null\n"
    "end function\n"
    // Fails, after enqueuing (which is dropped) and publishing (not sent).
    "job.flaky = function(args)\n"
    "    n = data.increment(\"flaky\")\n"
    "    enqueue \"record\", {\"what\": \"from flaky \" + n}\n"
    "    publish \"news\", \"flaky \" + n\n"
    "    return err(\"flaky failure \" + n)\n"
    "end function\n"
    "job.slow = function(args)\n"
    "    data.increment \"slowStarted\"\n"
    "    wait 3\n"
    "    data.increment \"slowFinished\"\n"
    "end function\n"
    "job.tick = function(args)\n"
    "    data.increment \"ticks\"\n"
    "end function\n";

std::string makeApp(const TempDir& dir, int feedPort = 0) {
    dir.write("app/private/config/app.ms",
              "config = {}\n"
              "config.jobs = {\"maxAttempts\": 3}\n" +
                  std::string(feedPort ? "config.outbound = {\"allowAddresses\": [\"127.0.0.1\"]}\n" : ""));
    dir.write("app/code/access.ms", "rules = {}\nrules[\"/live/news\"] = \"public\"\n"
                                    "rules[\"/live/members\"] = \"login\"\nrules[\"/live/staff\"] = \"admin\"\n");
    dir.write("app/code/jobs/work.ms", kJobs);
    dir.write("app/code/frags/item.html", "<li>{{title}}</li>\n");
    if (feedPort) {
        dir.write("app/code/outbound.ms", "allow = [\"http://127.0.0.1:" + std::to_string(feedPort) + "\"]\n");
        dir.write("app/code/jobs/feed.ms",
                  "job.pollFeed = function(args)\n"
                  "    resp = http.get(\"http://127.0.0.1:" + std::to_string(feedPort) + "/feed.json\")\n"
                  "    if resp isa error then return resp\n"
                  "    seen = data.get(\"seen\", [])\n"
                  "    for item in resp.json.items\n"
                  "        if seen.indexOf(item.id) != null then continue\n"
                  "        seen.push item.id\n"
                  "        publish \"news\", frag(\"item\", item)\n"
                  "    end for\n"
                  "    data.set \"seen\", seen\n"
                  "end function\n");
        dir.write("app/code/schedule.ms", "schedule = {}\nschedule[\"pollFeed\"] = \"every 1m\"\n");
    }
    dir.write("app/code/pages/index.html",
              "<!DOCTYPE html><html><head><title>Live</title></head><body>"
              "<ul hx-ext=\"sse\" sse-connect=\"/live/news\" sse-swap=\"message\" hx-swap=\"beforeend\"></ul>"
              "</body></html>");
    dir.write("app/code/pages/act.ms",
              "get.signup = function(req)\n"
              "    enqueue \"record\", {\"what\": \"welcome \" + req.query.who}\n"
              "    return \"ok\"\n"
              "end function\n"
              "get.chain = function(req)\n"
              "    enqueue \"record\", {\"what\": \"first\", \"next\": \"record\"}\n"
              "    return \"ok\"\n"
              "end function\n"
              // Enqueues, then fails: the job is dropped.
              "get.broken = function(req)\n"
              "    enqueue \"record\", {\"what\": \"should not run\"}\n"
              "    publish \"news\", \"should not be sent\"\n"
              "    x = 1 / undefinedThing\n"
              "end function\n"
              "get.digest = function(req)\n"
              "    enqueue \"record\", {\"what\": \"digest\"}, {\"key\": \"digest:ann\", \"delay\": 3600}\n"
              "    return \"ok\"\n"
              "end function\n"
              "get.flaky = function(req)\n"
              "    enqueue \"flaky\"\n"
              "    return \"ok\"\n"
              "end function\n"
              "get.slow = function(req)\n"
              "    enqueue \"slow\"\n"
              "    return \"ok\"\n"
              "end function\n"
              "get.announce = function(req)\n"
              "    publish \"news\", frag(\"item\", {\"title\": req.query.title})\n"
              "    publish \"members\", \"for members: \" + req.query.title, \"note\"\n"
              "    return \"sent\"\n"
              "end function\n"
              // Arguments must be inert, and the job must exist.
              "get.badArgs = function(req)\n"
              "    return str(enqueue(\"record\", {\"f\": @print}))\n"
              "end function\n"
              "get.noJob = function(req)\n"
              "    return str(enqueue(\"nosuch\"))\n"
              "end function\n"
              "get.badChannel = function(req)\n"
              "    return str(publish(\"no spaces\", \"x\"))\n"
              "end function\n"
              // A form rendered for this visitor carries their CSRF token.
              "get.leakToken = function(req)\n"
              "    return str(publish(\"news\", frag(\"form\")))\n"
              "end function\n");
    dir.write("app/code/frags/form.html", "<form method=\"post\" action=\"/act/x\"><button>Go</button></form>\n");
    return dir.at("app");
}

// A data value (data/<name>.json) or log (.jsonl), as written, trimmed.
std::string dataFile(const TempDir& dir, const std::string& name) {
    std::string s = readFile(dir.at("app/data/" + name + ".json")) + readFile(dir.at("app/data/" + name + ".jsonl"));
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

TEST_CASE("jobs: enqueued by a request, run in the background; failed requests' jobs dropped") {
    TempDir dir;
    std::string root = makeApp(dir);
    Server s(root);
    Browser b(s.port());

    CHECK(contains(b.get("/act/signup?who=ann"), "ok"));
    CHECK(eventually([&] { return contains(dataFile(dir, "log"), "welcome ann"); }));

    // A job's enqueues commit when it succeeds: "first", then "chained".
    CHECK(contains(b.get("/act/chain"), "ok"));
    CHECK(eventually([&] { return contains(dataFile(dir, "log"), "chained"); }));

    // A request that fails after enqueuing: nothing runs, nothing is sent.
    CHECK(statusOf(b.get("/act/broken")) == 500);
    CHECK(eventually([&] { return contains(s.err.str(), "jobs it enqueued were dropped (record)"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!contains(dataFile(dir, "log"), "should not run"));

    // Mistakes are errors in the request.
    CHECK(contains(b.get("/act/badArgs"), "only numbers, strings, lists and maps"));
    std::string noJob = b.get("/act/noJob");
    CHECK_MESSAGE(contains(noJob, "no job named nosuch"), noJob);
    CHECK(contains(b.get("/act/badChannel"), "channel name"));

    // The jobs panel shows what ran.
    std::string panel = b.get("/__dev/jobs");
    CHECK(statusOf(panel) == 200);
    CHECK(contains(panel, "record"));
    CHECK((contains(panel, "GET /act/signup") || contains(panel, "ok")));
    std::string json = b.get("/__dev/jobs.json");
    CHECK(contains(json, "\"runs\""));
    CHECK(contains(json, "\"known\""));
}

TEST_CASE("jobs: a key enqueues once while pending; delays") {
    TempDir dir;
    std::string root = makeApp(dir);
    Server s(root);
    Browser b(s.port());
    for (int i = 0; i < 3; i++) CHECK(contains(b.get("/act/digest"), "ok"));
    auto jobs = s.server->jobQueue().list().value();
    size_t digests = 0;
    for (const QueuedJob& j : jobs) digests += j.key == std::optional<std::string>("digest:ann");
    CHECK(digests == 1);
    // An hour away: it hasn't run.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!contains(dataFile(dir, "log"), "digest"));
}

TEST_CASE("jobs: a failing job is retried, then dead; its enqueues and messages are dropped") {
    TempDir dir;
    std::string root = makeApp(dir);
    Server s(root);
    Browser b(s.port());
    Subscriber news(s.port(), "/live/news");
    REQUIRE(news.waitFor("subscribed"));
    CHECK(contains(b.get("/act/flaky"), "ok"));
    // Three attempts (config.jobs.maxAttempts), 100 ms then 200 ms apart.
    CHECK(eventually([&] {
        std::vector<QueuedJob> list = s.server->jobQueue().list().value();
        for (const QueuedJob& j : list) {
            if (j.name == "flaky" && j.state == QueuedJob::State::Dead) return true;
        }
        return false;
    }));
    CHECK(dataFile(dir, "flaky") == "3");
    auto dead = s.server->jobQueue().list().value();
    REQUIRE(dead.size() == 1);
    CHECK(dead[0].attempts == 3);
    CHECK(contains(dead[0].lastError, "flaky failure 3"));
    CHECK(contains(s.out.str(), "Job flaky failed (attempt 3 of 3)"));
    CHECK(!contains(dataFile(dir, "log"), "from flaky"));
    CHECK(!contains(news.received(), "flaky"));

    // Retried from the panel: it fails again (a fourth time, as attempt 1).
    std::string origin = "Origin: http://localhost\r\n";
    std::string r = b.send("POST", "/__dev/jobs/retry", "id=" + std::to_string(dead[0].id), false, origin);
    CHECK(statusOf(r) == 303);
    CHECK(eventually([&] { return dataFile(dir, "flaky") == "6"; }));
    // Discarded.
    CHECK(eventually([&] {
        auto list = s.server->jobQueue().list().value();
        return !list.empty() && list[0].state == QueuedJob::State::Dead;
    }));
    r = b.send("POST", "/__dev/jobs/discard", "id=" + std::to_string(dead[0].id), false, origin);
    CHECK(statusOf(r) == 303);
    CHECK(s.server->jobQueue().list().value().empty());
    // Not from another site.
    CHECK(statusOf(b.send("POST", "/__dev/jobs/run", "job=tick", false, "Origin: http://evil.example\r\n")) == 403);
    // Run now.
    r = b.send("POST", "/__dev/jobs/run", "job=tick", false, origin);
    CHECK(statusOf(r) == 303);
    CHECK(eventually([&] { return dataFile(dir, "ticks") == "1"; }));
}

TEST_CASE("jobs: a job killed mid-run is retried when the engine starts again") {
    TempDir dir;
    std::string root = makeApp(dir);
    {
        Server s(root);
        Browser b(s.port());
        CHECK(contains(b.get("/act/slow"), "ok"));
        CHECK(eventually([&] { return dataFile(dir, "slowStarted") == "1"; }));
        // Stopped while it waits.
    }
    CHECK(dataFile(dir, "slowFinished").empty());
    {
        auto queue = JobQueue::open(dir.at("app/private/state/state.db")).value();
        auto list = queue->list().value();
        REQUIRE(list.size() == 1);
        CHECK(list[0].state == QueuedJob::State::Running);
    }
    Server s(root);
    CHECK(eventually([&] { return dataFile(dir, "slowFinished") == "1"; }));
    CHECK(dataFile(dir, "slowStarted") == "2");
    CHECK(s.server->jobQueue().list().value().empty());
}

TEST_CASE("schedules: missed runs run once after downtime") {
    TempDir dir;
    std::string root = makeApp(dir);
    dir.write("app/code/schedule.ms", "schedule = {}\nschedule[\"tick\"] = \"every 5m\"\n");
    {
        // As if the engine had last run it three hours ago, and been down
        // since: 36 ticks missed.
        std::filesystem::create_directories(dir.at("app/private/state"));
        auto queue = JobQueue::open(dir.at("app/private/state/state.db")).value();
        int64_t now = systemClock().nowMillis();
        REQUIRE(queue->saveSchedule({"tick", "every 5m @ UTC", now - 3 * 3600'000, now - 3 * 3600'000 + 300'000}));
    }
    Server s(root);
    CHECK(eventually([&] { return dataFile(dir, "ticks") == "1"; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(dataFile(dir, "ticks") == "1");
    auto states = s.server->jobQueue().schedules().value();
    int64_t now = systemClock().nowMillis();
    CHECK(states["tick"].nextRunMs > now + 4 * 60'000);
    CHECK(states["tick"].nextRunMs <= now + 5 * 60'000);
    // The panel shows the schedule.
    Browser b(s.port());
    std::string panel = b.get("/__dev/jobs");
    CHECK(contains(panel, "every 5m"));
    // Paused: the schedule shows it.
    CHECK(statusOf(b.send("POST", "/__dev/jobs/pause", "", false, "Origin: http://localhost\r\n")) == 303);
    CHECK(s.server->jobs().paused());
    CHECK(contains(b.get("/__dev/jobs"), "(paused)"));
}

TEST_CASE("push: requests publish to a channel's subscribers; access rules apply") {
    TempDir dir;
    std::string root = makeApp(dir);
    Server s(root);
    Browser b(s.port());

    // HTMX's SSE extension comes with htmx.
    CHECK(contains(b.get("/__fourfold/htmx.js"), "defineExtension('sse'"));

    Subscriber news(s.port(), "/live/news");
    REQUIRE(news.waitFor("subscribed to news"));
    CHECK(contains(news.received(), "Content-Type: text/event-stream"));
    CHECK(contains(b.get("/act/announce?title=%3Cb%3EHi%3C%2Fb%3E"), "sent"));
    CHECK(news.waitFor("event: message\ndata: <li>&lt;b&gt;Hi&lt;/b&gt;</li>\n\n"));

    // Members only: refused without signing in, and allowed after.
    std::string anon = test::rawFetch(s.port(), "GET /live/members HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(statusOf(anon) == 401);
    CHECK(!contains(anon, "event-stream"));
    Browser member(s.port());
    fakeSignIn(member, "member");
    Subscriber members(s.port(), "/live/members", cookieHeader(member));
    REQUIRE(members.waitFor("subscribed to members"));
    // A member isn't an admin.
    Subscriber staff(s.port(), "/live/staff", cookieHeader(member));
    CHECK(staff.waitFor("HTTP/1.1 403"));
    CHECK(contains(b.get("/act/announce?title=again"), "sent"));
    CHECK(members.waitFor("event: note\ndata: for members: again"));
    CHECK(news.waitFor("<li>again</li>"));
    CHECK(!contains(news.received(), "for members"));

    // A channel no rule covers isn't served; nor is a POST.
    CHECK(statusOf(b.get("/live/elsewhere")) == 404);
    CHECK(statusOf(b.send("POST", "/live/news", "")) == 405);

    // A failed request publishes nothing.
    CHECK(statusOf(b.get("/act/broken")) == 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!contains(news.received(), "should not be sent"));

    // A form with this visitor's CSRF token can't go to every subscriber.
    CHECK(contains(b.get("/act/leakToken"), "CSRF token"));

    // The jobs panel lists open channels; the access report, the rules.
    CHECK(contains(b.get("/__dev/jobs"), "/live/news: 1"));
    std::string access = b.get("/__dev/access");
    CHECK(contains(access, "/live/members/*"));
    CHECK(contains(access, "push channel"));
}

TEST_CASE("push: a feed-polling job publishes new items to open pages") {
    std::atomic<int> polls{0};
    FakeHttp feed([&](const FakeHttp::Request& r) {
        if (r.target() != "/feed.json") return FakeHttp::response(404, "nope");
        int n = ++polls;
        std::string items = "{\"id\": 1, \"title\": \"First story\"}";
        if (n > 1) items += ", {\"id\": 2, \"title\": \"Second story\"}";
        return FakeHttp::response(200, "{\"items\": [" + items + "]}", "Content-Type: application/json\r\n");
    });
    TempDir dir;
    std::string root = makeApp(dir, feed.port());
    Server s(root);
    Browser b(s.port());
    // The page subscribes as a browser would, through its sse-connect.
    std::string page = b.get("/");
    CHECK(contains(page, "sse-connect=\"/live/news\""));
    Subscriber reader(s.port(), "/live/news");
    REQUIRE(reader.waitFor("subscribed"));
    // The schedule ran it at start (an "every" schedule's first run is at
    // once); run it again now, as the next tick would.
    CHECK(eventually([&] { return polls >= 1; }));
    std::string origin = "Origin: http://localhost\r\n";
    CHECK(eventually([&] {
        b.send("POST", "/__dev/jobs/run", "job=pollFeed", false, origin);
        return reader.waitFor("<li>Second story</li>", 1);
    }));
    std::string got = reader.received();
    // Each item once.
    size_t second = 0;
    for (size_t at = got.find("Second story"); at != std::string::npos; at = got.find("Second story", at + 1)) second++;
    CHECK(second == 1);
}

TEST_CASE("the newsfeed sample compiles and serves, with its schedule in its time zone") {
    TempDir dir;
    std::filesystem::path source = std::filesystem::path(FOURFOLD_TESTS_DIR) / ".." / "examples" / "newsfeed";
    std::filesystem::create_directories(dir.at("app"));
    std::filesystem::copy(source / "code", dir.at("app/code"), std::filesystem::copy_options::recursive);
    std::filesystem::create_directories(dir.at("app/private/config"));
    std::filesystem::copy(source / "private" / "config" / "app.ms", dir.at("app/private/config/app.ms"));
    // No requests to the real API from tests: nothing is allowed out, and
    // only the digest is scheduled.
    dir.write("app/code/outbound.ms", "mailTo = [\"owner@example.com\"]\n");
    dir.write("app/code/schedule.ms", "schedule = {}\nschedule[\"dailyDigest\"] = \"daily 07:00\"\n");
    dir.write("app/data/stories.json",
              "[{\"id\": 1, \"title\": \"A <story>\", \"url\": \"https://example.com/1\", \"score\": 5, \"by\": \"ann\"}]\n");
    Server s(dir.at("app"));
    Browser b(s.port());
    std::string page = b.get("/");
    CHECK(contains(page, "A &lt;story&gt;"));
    CHECK(contains(page, "sse-connect=\"/live/stories\""));
    Subscriber reader(s.port(), "/live/stories");
    CHECK(reader.waitFor("subscribed to stories"));
    std::string panel = b.get("/__dev/jobs");
    CHECK(contains(panel, "America/Denver"));
    CHECK(contains(panel, "07:00:00 America/Denver"));
    // The digest, run now, mails the owner (printed, in dev mode).
    CHECK(statusOf(b.send("POST", "/__dev/jobs/run", "job=dailyDigest", false, "Origin: http://localhost\r\n")) == 303);
    CHECK(eventually([&] { return contains(s.out.str(), "Job dailyDigest ran"); }));
    CHECK_MESSAGE(contains(s.out.str(), "story\\>](https://example.com/1) (5 points)"), s.out.str());
}

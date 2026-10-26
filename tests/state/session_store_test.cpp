#include "state/session_store.h"

#include <fstream>
#include <sstream>

#include "base/crypto.h"
#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

struct Fixture {
    TempDir dir;
    ManualClock clock;
    std::unique_ptr<SessionStore> store;

    explicit Fixture(SessionConfig config = {}) {
        store = SessionStore::open(dir.at("state.db"), config, clock).value();
    }
    std::string make(const std::string& group = "10.0.0.1", const std::string& app = "{}") {
        std::string id = SessionStore::newId();
        REQUIRE(store->create(id, group, "{}", app));
        return id;
    }
    bool live(const std::string& id) {
        auto r = store->load(*SessionStore::hashOfId(id));
        REQUIRE(r);
        return r.value().has_value();
    }
};

}  // namespace

TEST_CASE("SessionStore: IDs and their hashes") {
    std::string id = SessionStore::newId();
    CHECK_EQ(id.size(), 43u);
    CHECK_NE(id, SessionStore::newId());
    auto hash = SessionStore::hashOfId(id);
    REQUIRE(hash);
    CHECK_EQ(*hash, hashBytes(*fromBase64Url(id)));
    CHECK_FALSE(SessionStore::hashOfId(""));
    CHECK_FALSE(SessionStore::hashOfId(id.substr(1)));
    CHECK_FALSE(SessionStore::hashOfId(std::string(43, '*')));
}

TEST_CASE("SessionStore: create, load, save, remove") {
    Fixture f;
    std::string id = f.make("10.0.0.1", "{\"cart\":[1]}");
    std::string hash = *SessionStore::hashOfId(id);
    auto r = f.store->load(hash).value();
    REQUIRE(r);
    CHECK_EQ(r->app, "{\"cart\":[1]}");
    CHECK_EQ(r->engine, "{}");
    CHECK_FALSE(r->userId);
    CHECK_EQ(r->createdAt, f.clock.nowMillis());

    f.clock.advanceSeconds(10);
    REQUIRE(f.store->save(hash, "{\"user\":\"u1\"}", "{\"cart\":[1,2]}"));
    r = f.store->load(hash).value();
    CHECK_EQ(r->app, "{\"cart\":[1,2]}");
    CHECK_EQ(r->engine, "{\"user\":\"u1\"}");
    CHECK_EQ(r->lastUsedAt, f.clock.nowMillis());

    REQUIRE(f.store->remove(hash));
    CHECK_FALSE(f.store->load(hash).value());
    CHECK(f.store->save(hash, "{}", "{}").error().code == ErrorCode::NotFound);
}

TEST_CASE("SessionStore: the raw ID is never stored") {
    Fixture f;
    std::string id = f.make();
    std::string raw = *fromBase64Url(id);
    f.store.reset();   // close, so the file is complete
    std::ifstream in(f.dir.at("state.db"), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string bytes = ss.str();
    std::ifstream wal(f.dir.at("state.db-wal"), std::ios::binary);
    std::stringstream ws;
    ws << wal.rdbuf();
    bytes += ws.str();
    CHECK(bytes.find(raw) == std::string::npos);
    CHECK(bytes.find(id) == std::string::npos);
    CHECK(bytes.find(hashBytes(raw)) != std::string::npos);
}

TEST_CASE("SessionStore: idle and absolute timeouts") {
    Fixture f;
    std::string id = f.make();
    f.clock.advanceSeconds(23 * 3600);
    CHECK(f.live(id));
    f.clock.advanceSeconds(2 * 3600);   // 25 h idle
    CHECK_FALSE(f.live(id));

    // Kept alive by use, but not past the absolute limit.
    std::string id2 = f.make();
    std::string hash2 = *SessionStore::hashOfId(id2);
    for (int day = 0; day < 6; day++) {
        f.clock.advanceSeconds(20 * 3600);
        auto r = f.store->load(hash2).value();
        REQUIRE(r);
        REQUIRE(f.store->touch(hash2, r->lastUsedAt));
    }
    f.clock.advanceSeconds(20 * 3600);   // 140 h old
    CHECK(f.live(id2));
    f.clock.advanceSeconds(30 * 3600);   // 170 h > 7 days
    CHECK_FALSE(f.live(id2));
}

TEST_CASE("SessionStore: touch writes at most every touchInterval") {
    Fixture f;
    std::string hash = *SessionStore::hashOfId(f.make());
    int64_t created = f.clock.nowMillis();
    f.clock.advanceSeconds(60);
    REQUIRE(f.store->touch(hash, created));
    CHECK_EQ(f.store->load(hash).value()->lastUsedAt, created);   // too soon
    f.clock.advanceSeconds(300);
    REQUIRE(f.store->touch(hash, created));
    CHECK_EQ(f.store->load(hash).value()->lastUsedAt, f.clock.nowMillis());
}

TEST_CASE("SessionStore: caps evict the oldest anonymous sessions") {
    SessionConfig config;
    config.maxAnonymousPerAddress = 3;
    config.maxAnonymous = 5;
    Fixture f(config);
    std::vector<std::string> a;
    for (int i = 0; i < 4; i++) {
        a.push_back(f.make("1.1.1.1"));
        f.clock.advanceSeconds(1);
    }
    CHECK_FALSE(f.live(a[0]));   // the oldest of that address went
    CHECK(f.live(a[1]));
    CHECK(f.live(a[3]));
    std::vector<std::string> b;
    for (int i = 0; i < 3; i++) {
        b.push_back(f.make("2.2.2." + std::to_string(i)));
        f.clock.advanceSeconds(1);
    }
    CHECK_EQ(f.store->count().value(), 5);
    CHECK_FALSE(f.live(a[1]));   // the oldest overall went
    CHECK(f.live(b[2]));
}

TEST_CASE("SessionStore: sweepExpired") {
    Fixture f;
    f.make();
    f.make();
    f.clock.advanceSeconds(25 * 3600);
    std::string fresh = f.make();
    CHECK_EQ(f.store->sweepExpired().value(), 2);
    CHECK(f.live(fresh));
    CHECK_EQ(f.store->count().value(), 1);
}

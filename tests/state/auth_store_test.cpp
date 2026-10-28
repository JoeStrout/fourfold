#include "state/auth_store.h"

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

struct Fixture {
    TempDir dir;
    ManualClock clock;
    std::unique_ptr<AuthStore> store;

    explicit Fixture(AuthStoreConfig config = {}) {
        store = AuthStore::open(dir.at("state.db"), config, clock).value();
    }
};

}  // namespace

TEST_CASE("AuthStore: users and roles") {
    Fixture f;
    auto ann = f.store->create("  Ann@Example.COM ", "Ann");
    REQUIRE(ann);
    CHECK_EQ(ann.value().email, "ann@example.com");
    CHECK_EQ(ann.value().id.size(), 22u);
    CHECK_FALSE(f.store->create("ann@example.com", "Again"));

    auto found = f.store->findByEmail("ANN@example.com").value();
    REQUIRE(found);
    CHECK_EQ(found->id, ann.value().id);
    CHECK(found->roles.empty());

    REQUIRE(f.store->grant(ann.value().id, "moderator"));
    REQUIRE(f.store->grant(ann.value().id, "admin"));
    REQUIRE(f.store->grant(ann.value().id, "admin"));   // already has it: fine
    auto got = f.store->get(ann.value().id).value();
    REQUIRE(got);
    CHECK_EQ(got->roles, std::vector<std::string>{"admin", "moderator"});
    REQUIRE(f.store->revoke(ann.value().id, "admin"));
    CHECK_EQ(f.store->get(ann.value().id).value()->roles, std::vector<std::string>{"moderator"});

    CHECK(f.store->grant("nobody", "admin").error().code == ErrorCode::NotFound);
    CHECK_FALSE(f.store->get("nobody").value());
    REQUIRE(f.store->setName(ann.value().id, "Ann B."));
    CHECK_EQ(f.store->get(ann.value().id).value()->name, "Ann B.");
    CHECK_EQ(f.store->all().value().size(), 1u);
}

TEST_CASE("AuthStore: a token works once, and only for a while") {
    Fixture f;
    auto token = f.store->createToken("Bob@example.com", "10.0.0.1", "/members").value();
    REQUIRE(token);
    auto used = f.store->redeemToken(*token).value();
    REQUIRE(used);
    CHECK_EQ(used->email, "bob@example.com");
    CHECK_EQ(used->next, "/members");
    CHECK_FALSE(f.store->redeemToken(*token).value());   // used up

    auto late = f.store->createToken("bob@example.com", "10.0.0.1", "/").value();
    REQUIRE(late);
    f.clock.advanceSeconds(16 * 60);
    CHECK_FALSE(f.store->redeemToken(*late).value());   // expired

    CHECK_FALSE(f.store->redeemToken("not a token").value());
    CHECK_FALSE(f.store->redeemToken(std::string(43, 'A')).value());
}

TEST_CASE("AuthStore: tokens are throttled per email and per address") {
    Fixture f;
    for (int i = 0; i < 3; i++) CHECK(f.store->createToken("c@example.com", "10.0.0.1", "/").value());
    CHECK_FALSE(f.store->createToken("C@example.com", "10.0.0.2", "/").value());
    for (int i = 0; i < 7; i++) CHECK(f.store->createToken("d" + std::to_string(i) + "@x.com", "10.0.0.1", "/").value());
    CHECK_FALSE(f.store->createToken("e@example.com", "10.0.0.1", "/").value());
    CHECK(f.store->createToken("e@example.com", "10.0.0.3", "/").value());

    f.clock.advanceSeconds(3601);
    CHECK(f.store->createToken("c@example.com", "10.0.0.1", "/").value());
    CHECK(f.store->sweep().value() >= 11);
}

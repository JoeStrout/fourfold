#include "base/result.h"

#include <memory>

#include "doctest.h"

using namespace fourfold;

namespace {

Result<int> half(int n) {
    if (n % 2 != 0) return makeError(ErrorCode::Invalid, "odd number");
    return n / 2;
}

}  // namespace

TEST_CASE("Result holds a value") {
    Result<int> r = half(10);
    REQUIRE(r.ok());
    CHECK(static_cast<bool>(r));
    CHECK_EQ(r.value(), 5);
}

TEST_CASE("Result holds an error") {
    Result<int> r = half(3);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Invalid);
    CHECK_EQ(r.error().message, std::string("odd number"));
    CHECK_EQ(r.error().toString(), std::string("Invalid: odd number"));
}

TEST_CASE("Result can hold a move-only value") {
    Result<std::unique_ptr<int>> r = std::make_unique<int>(7);
    REQUIRE(r.ok());
    std::unique_ptr<int> p = std::move(r).value();
    CHECK_EQ(*p, 7);
}

TEST_CASE("Status reports success or an error") {
    Status good;
    CHECK(good.ok());
    Status bad = makeError(ErrorCode::NotFound, "");
    REQUIRE_FALSE(bad.ok());
    CHECK_EQ(bad.error().toString(), std::string("NotFound"));
}

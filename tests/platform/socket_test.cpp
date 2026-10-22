#include "platform/socket.h"

#include "doctest.h"

using namespace fourfold;

TEST_CASE("Fd closes on destruction and moves ownership") {
    auto pair = makeSocketPair();
    REQUIRE(pair.ok());
    Fd a = std::move(pair.value().first);
    int raw = a.get();
    CHECK(a.valid());
    Fd b = std::move(a);
    CHECK_FALSE(a.valid());
    CHECK_EQ(b.get(), raw);
    CHECK_EQ(b.release(), raw);
    CHECK_FALSE(b.valid());
    Fd(raw).reset();   // close it ourselves, since we released it
}

TEST_CASE("readSome and writeSome on a socket pair") {
    auto pair = makeSocketPair();
    REQUIRE(pair.ok());
    auto& [a, b] = pair.value();
    char buffer[16];

    auto empty = readSome(a.get(), buffer, sizeof buffer);
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().code == ErrorCode::WouldBlock);

    auto wrote = writeSome(b.get(), "hello", 5);
    REQUIRE(wrote.ok());
    CHECK_EQ(wrote.value(), 5u);
    auto got = readSome(a.get(), buffer, sizeof buffer);
    REQUIRE(got.ok());
    CHECK_EQ(std::string(buffer, got.value()), "hello");

    b.reset();
    auto eof = readSome(a.get(), buffer, sizeof buffer);
    REQUIRE(eof.ok());
    CHECK_EQ(eof.value(), 0u);

    // Writing to a closed peer is an error, not SIGPIPE.
    auto broken = writeSome(a.get(), "x", 1);
    CHECK_FALSE(broken.ok());
}

TEST_CASE("listenTcp rejects a non-numeric host") {
    auto fd = listenTcp("localhost", 0);
    REQUIRE_FALSE(fd.ok());
    CHECK(fd.error().code == ErrorCode::Invalid);
}

TEST_CASE("listenTcp on port 0 picks a free port") {
    auto fd = listenTcp("127.0.0.1", 0);
    if (!fd.ok() && fd.error().code == ErrorCode::Denied) {
        MESSAGE("skipped: binding a local port isn't permitted here (" << fd.error().toString() << ")");
        return;
    }
    REQUIRE(fd.ok());
    auto port = localPort(fd.value().get());
    REQUIRE(port.ok());
    CHECK(port.value() != 0);

    auto none = acceptConnection(fd.value().get());
    REQUIRE(none.ok());
    CHECK_FALSE(none.value().has_value());   // nothing waiting
}

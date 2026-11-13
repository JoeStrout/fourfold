#include "platform/socket.h"
#include "platform/address.h"

#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

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

TEST_CASE("AddressRange: addresses and CIDR ranges, IPv4 and IPv6") {
    auto r = fourfold::AddressRange::parse("10.0.0.0/8");
    REQUIRE(r);
    CHECK(r->contains("10.1.2.3"));
    CHECK(r->contains("::ffff:10.1.2.3"));   // IPv4-mapped
    CHECK_FALSE(r->contains("11.0.0.1"));
    CHECK_FALSE(r->contains("garbage"));
    auto one = fourfold::AddressRange::parse("127.0.0.1");
    REQUIRE(one);
    CHECK(one->contains("127.0.0.1"));
    CHECK_FALSE(one->contains("127.0.0.2"));
    auto v6 = fourfold::AddressRange::parse("2001:db8::/32");
    REQUIRE(v6);
    CHECK(v6->contains("2001:db8:1::5"));
    CHECK_FALSE(v6->contains("2001:db9::5"));
    CHECK(fourfold::AddressRange::parse("::1")->contains("[::1]"));
    for (const char* bad : {"", "10.0.0.0/33", "::/129", "10.0.0/8", "x/8", "10.0.0.0/"}) {
        CAPTURE(bad);
        CHECK_FALSE(fourfold::AddressRange::parse(bad));
    }
}

TEST_CASE("takeSystemdSockets: only this process's, with their names, and the variables go") {
    setenv("LISTEN_PID", "1", 1);   // not this process
    setenv("LISTEN_FDS", "1", 1);
    setenv("LISTEN_FDNAMES", "https", 1);
    CHECK(takeSystemdSockets().empty());
    CHECK(getenv("LISTEN_PID") == nullptr);
    CHECK(getenv("LISTEN_FDS") == nullptr);
    CHECK(getenv("LISTEN_FDNAMES") == nullptr);
    CHECK(takeSystemdSockets().empty());   // nothing set

    // As systemd does it: listening sockets at fds 3 and 4, in a child.
    auto https = listenTcp("127.0.0.1", 0);
    auto http = listenTcp("127.0.0.1", 0);
    REQUIRE(https.ok());
    REQUIRE(http.ok());
    uint16_t httpsPort = localPort(https.value().get()).value();
    uint16_t httpPort = localPort(http.value().get()).value();
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        int a = dup(https.value().get()), b = dup(http.value().get());
        dup2(a, 3);
        dup2(b, 4);
        setenv("LISTEN_PID", std::to_string(getpid()).c_str(), 1);
        setenv("LISTEN_FDS", "2", 1);
        setenv("LISTEN_FDNAMES", "https:http", 1);
        auto got = takeSystemdSockets();
        bool ok = got.size() == 2 && got[0].name == "https" && got[1].name == "http" &&
                  localPort(got[0].fd.get()).value() == httpsPort && localPort(got[1].fd.get()).value() == httpPort &&
                  getenv("LISTEN_FDS") == nullptr;
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

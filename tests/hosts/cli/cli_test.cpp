#include "cli.h"

#include <sstream>

#include "doctest.h"

using fourfold::cli::run;

TEST_CASE("cli: version prints the version") {
    std::ostringstream out, err;
    CHECK_EQ(run({"version"}, out, err), 0);
    CHECK_EQ(out.str().rfind("fourfold ", 0), size_t(0));
    CHECK(err.str().empty());
}

TEST_CASE("cli: no arguments is a usage error") {
    std::ostringstream out, err;
    CHECK_EQ(run({}, out, err), 2);
    CHECK(err.str().find("Usage:") != std::string::npos);
}

TEST_CASE("cli: unknown command is a usage error") {
    std::ostringstream out, err;
    CHECK_EQ(run({"frobnicate"}, out, err), 2);
    CHECK(err.str().find("unknown command 'frobnicate'") != std::string::npos);
}

TEST_CASE("cli: dev argument errors are usage errors") {
    for (std::vector<std::string> args : {
             std::vector<std::string>{"dev"},
             std::vector<std::string>{"dev", "a", "b"},
             std::vector<std::string>{"dev", "app", "--port"},
             std::vector<std::string>{"dev", "app", "--port", "0"},
             std::vector<std::string>{"dev", "app", "--port", "65536"},
             std::vector<std::string>{"dev", "app", "--port", "80x"},
             std::vector<std::string>{"dev", "app", "--frobnicate"},
         }) {
        std::ostringstream out, err;
        CHECK_EQ(run(args, out, err), 2);
        CHECK(err.str().find("Usage: fourfold dev") != std::string::npos);
    }
}

TEST_CASE("cli: serve argument errors are usage errors") {
    for (std::vector<std::string> args : {
             std::vector<std::string>{"serve"},
             std::vector<std::string>{"serve", "app", "--listen"},
             std::vector<std::string>{"serve", "app", "--listen", "8090"},
             std::vector<std::string>{"serve", "app", "--listen", ":8090"},
             std::vector<std::string>{"serve", "app", "--listen", "0.0.0.0:0"},
             std::vector<std::string>{"serve", "app", "--server-config"},
             std::vector<std::string>{"serve", "app", "--host", "a.com:80"},
             std::vector<std::string>{"serve", "app", "--frobnicate"},
         }) {
        std::ostringstream out, err;
        CHECK_EQ(run(args, out, err), 2);
        CHECK(err.str().find("Usage: fourfold serve") != std::string::npos);
    }
}

TEST_CASE("cli: front, dropbox-writer and serve's Unix socket: argument errors") {
    struct Case {
        std::vector<std::string> args;
        const char* says;
    };
    for (const Case& c : {
             Case{{"serve", "app", "--listen", "unix:relative.sock"}, "needs an absolute path"},
             Case{{"serve", "app", "--tls", "--listen", "unix:/run/x.sock"}, "don't go together"},
             Case{{"serve", "app", "--dropbox-socket"}, "needs a socket path"},
             Case{{"front"}, "--server-config"},
             Case{{"front", "--server-config"}, "--server-config needs a value"},
             Case{{"front", "--server-config", "s.ms", "--listen", "443"}, "ADDRESS:PORT"},
             Case{{"front", "--server-config", "s.ms", "--bogus"}, "unknown argument"},
             Case{{"dropbox-writer"}, "give the app's dropbox/ folder"},
             Case{{"dropbox-writer", "dropbox"}, "--listen"},
             Case{{"dropbox-writer", "dropbox", "--bogus"}, "unknown argument"},
         }) {
        std::ostringstream out, err;
        CHECK_MESSAGE(run(c.args, out, err) == 2, c.args[0], " ", c.says);
        CHECK_MESSAGE(err.str().find(c.says) != std::string::npos, err.str());
    }
    // A server config that can't be read is an error, not a usage error.
    std::ostringstream out, err;
    CHECK_EQ(run({"front", "--server-config", "/nonexistent/server.ms"}, out, err), 1);
    CHECK(err.str().find("fourfold front:") != std::string::npos);
}

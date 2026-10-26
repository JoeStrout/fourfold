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
             std::vector<std::string>{"serve", "app", "--listen", "0.0.0.0:8090"},   // no --host
             std::vector<std::string>{"serve", "app", "--host", "a.com:80"},
             std::vector<std::string>{"serve", "app", "--frobnicate"},
         }) {
        std::ostringstream out, err;
        CHECK_EQ(run(args, out, err), 2);
        CHECK(err.str().find("Usage: fourfold serve") != std::string::npos);
    }
}

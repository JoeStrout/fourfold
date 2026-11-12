#include "platform/ca_bundle.h"

#include <cstdlib>

#include "doctest.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

TEST_CASE("caBundleFallback: only when the built-in file is missing") {
    TempDir dir;
    dir.write("ours.pem", "certificates");
    CHECK_FALSE(caBundleFallback(nullptr));                      // the TLS library's own store
    CHECK_FALSE(caBundleFallback(dir.at("ours.pem").c_str()));   // there: left alone
    ::setenv("SSL_CERT_FILE", dir.at("ours.pem").c_str(), 1);
    auto found = caBundleFallback(dir.at("missing.pem").c_str());
    REQUIRE(found);
    CHECK(*found == dir.at("ours.pem"));
    ::unsetenv("SSL_CERT_FILE");
}

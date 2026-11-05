// A Postgres database for tests, if one is provided: FOURFOLD_TEST_PG_URL
// names a server (e.g. "postgres://localhost/fourfold_test"). Each test
// works in a schema of its own, made for it and dropped afterward, so tests
// never touch anything else in that database. Without the variable, the
// Postgres tests are skipped (with a message saying so).
//
// Header-only: include it only from tests that link appdata.
#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "appdata/database.h"
#include "base/crypto.h"
#include "platform/random.h"

namespace fourfold::test {

class TestPostgres {
public:
    // nullopt if no server is configured (or this build has no Postgres).
    static std::optional<PostgresSettings> settings() {
        const char* url = std::getenv("FOURFOLD_TEST_PG_URL");
        if (!url || !*url || !Database::postgresSupported()) return std::nullopt;
        PostgresSettings s;
        s.url = url;
        return s;
    }

    // Makes a fresh schema; `ok()` says whether it worked.
    TestPostgres() {
        auto base = settings();
        if (!base) return;
        settings_ = *base;
        settings_.schema = "ff_test_" + toHex(randomBytes(6));
        auto admin = Database::openPostgres(*base);
        if (!admin) {
            error_ = admin.error().message;
            return;
        }
        auto conn = admin.value()->connection();
        if (!conn || !conn.value()->exec("CREATE SCHEMA " + settings_.schema)) {
            error_ = "can't create a schema for the test";
            return;
        }
        admin_ = std::move(admin).value();
        ok_ = true;
    }

    ~TestPostgres() {
        if (!ok_) return;
        admin_->closeIdle();
        auto conn = admin_->connection();
        if (conn) (void)conn.value()->exec("DROP SCHEMA " + settings_.schema + " CASCADE");
    }

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    // Connection settings that work in the test's schema.
    const PostgresSettings& config() const { return settings_; }

private:
    PostgresSettings settings_;
    std::unique_ptr<Database> admin_;
    bool ok_ = false;
    std::string error_;
};

}  // namespace fourfold::test

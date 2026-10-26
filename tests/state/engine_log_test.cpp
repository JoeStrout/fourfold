#include "state/engine_log.h"

#include <fstream>
#include <sstream>

#include "doctest.h"
#include "partition/app_folder.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("EngineLog: one line per entry, one file per day, control characters escaped") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    ManualClock clock(1'761'436'262'123);   // 2025-10-25T23:51:02.123Z
    EngineLog log(app.stateDir().value(), clock);
    log.write("start", "serving");
    log.write("csrf", "POST /x: bad token\n2025-10-25T00:00:00.000Z fake line\x01");
    log.flush();
    CHECK_EQ(log.currentFile(), "logs/engine-2025-10-25.log");
    std::string text = slurp(dir.at("app/private/state/logs/engine-2025-10-25.log"));
    CHECK_EQ(text,
             "2025-10-25T23:51:02.123Z start serving\n"
             "2025-10-25T23:51:02.123Z csrf POST /x: bad token\\n2025-10-25T00:00:00.000Z fake line\\x01\n");
    clock.advanceSeconds(3600);
    log.write("request", "next day");
    log.flush();
    CHECK_EQ(slurp(dir.at("app/private/state/logs/engine-2025-10-26.log")),
             "2025-10-26T00:51:02.123Z request next day\n");
}

TEST_CASE("Engine key: made once, then the same") {
    TempDir dir;
    auto app = AppFolder::open(dir.at("app"), AppFolder::Missing::Create).value();
    StateDir state = app.stateDir().value();
    auto first = loadOrCreateEngineKey(state);
    REQUIRE(first);
    CHECK_EQ(first.value().size(), 32u);
    auto second = loadOrCreateEngineKey(state);
    REQUIRE(second);
    CHECK_EQ(first.value(), second.value());
    REQUIRE(state.write(RelPath::parse("secret.key").value(), "short"));
    CHECK_FALSE(loadOrCreateEngineKey(state));
    CHECK(state.create(RelPath::parse("secret.key").value(), "x").error().code == ErrorCode::Conflict);
}

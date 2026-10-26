#include "base/clock.h"

#include <chrono>

namespace fourfold {

namespace {

class SystemClock : public Clock {
public:
    int64_t nowMillis() const override {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};

}  // namespace

const Clock& systemClock() {
    static const SystemClock clock;
    return clock;
}

}  // namespace fourfold

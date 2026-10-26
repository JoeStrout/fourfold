// TestGate: approvals for tests that run handlers directly, without the
// engine's PolicyGate. Only tests may include this (Arch §7: in Fourfold
// itself, only PolicyGate makes a GateApproval).
#pragma once

#include "script/script_host.h"

namespace fourfold {

struct TestGate {
    static GateApproval approve() { return GateApproval(); }
};

}  // namespace fourfold

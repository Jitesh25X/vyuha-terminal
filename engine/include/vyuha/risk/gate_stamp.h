#pragma once

#include <cstdint>

#include "vyuha/core/clock.h"

namespace vyuha::risk {

// What the gate records with each decision (RISK_ENGINE §1): the limit set it used, how long it took and
// when it ran. The APPROVED event carries it into order_event.payload.
struct GateStamp {
  std::uint64_t ruleset_version{0};  // LimitSet::version
  std::int64_t eval_ns{0};
  Nanos ts{0};
  bool operator==(const GateStamp&) const = default;
};

}  // namespace vyuha::risk

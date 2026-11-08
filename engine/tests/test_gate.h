#pragma once

// An approval for tests that need one, from a real RiskGate with no rules: it passes paper orders and
// refuses live ones (a gate missing rules must not approve real orders). Tests never construct an
// ApprovedOrder any other way, because nothing can. Pass the OMS's clock: the router refuses a stale stamp.

#include <utility>

#include "vyuha/core/clock.h"
#include "vyuha/risk/risk_gate.h"

namespace vyuha::testing {

inline risk::ApprovedOrder approve(const oms::OrderDetails& details, const IClock& clock) {
  const risk::RiskGate gate = risk::RiskGate::create({}, clock).value();
  risk::GateDecision decision = gate.evaluate({}, details);
  return std::move(decision.approved).value();  // throws on a live order: the test is wrong
}

}  // namespace vyuha::testing

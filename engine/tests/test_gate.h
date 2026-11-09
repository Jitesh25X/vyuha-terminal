#pragma once

// An approval for tests that need one, from a real RiskGate with no rules: it passes paper orders and
// refuses live ones (a gate missing rules must not approve real orders). Tests never construct an
// ApprovedOrder any other way, because nothing can. Pass the OMS's clock: the router refuses a stale stamp.

#include <cstdint>
#include <utility>

#include "vyuha/core/clock.h"
#include "vyuha/oms/order_router.h"
#include "vyuha/risk/risk_gate.h"

namespace vyuha::testing {

// A send queue with no account table: every account has the highest cap, and all orders use one exchange.
class FixedSendLimits final : public oms::ISendLimits {
 public:
  explicit FixedSendLimits(std::uint8_t cap = oms::kMaxOpsCap) noexcept : cap_(cap) {}
  [[nodiscard]] std::uint8_t ops_cap(const oms::Uuid& /*account*/) const noexcept override { return cap_; }
  [[nodiscard]] std::uint8_t ops_venue(std::int64_t /*instrument_id*/) const noexcept override { return 0; }

 private:
  std::uint8_t cap_;
};
inline const FixedSendLimits& open_limits() {
  static const FixedSendLimits limits;
  return limits;
}

inline risk::ApprovedOrder approve(const oms::OrderDetails& details, const IClock& clock) {
  const risk::RiskGate gate = risk::RiskGate::create({}, clock).value();
  risk::GateContext ctx;
  risk::GateDecision decision = gate.evaluate(ctx, details);
  return std::move(decision.approved).value();  // throws on a live order: the test is wrong
}

}  // namespace vyuha::testing

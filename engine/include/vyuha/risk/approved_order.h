#pragma once

#include "vyuha/oms/oms.h"
#include "vyuha/risk/gate_stamp.h"

namespace vyuha::risk {

class RiskGate;

// Proof that an order passed the RiskGate (invariant 1). Only RiskGate can make one, and order senders take
// nothing else, so no order reaches a broker without the gate. It can't be copied: an approval is handed on,
// not duplicated. Moving one empties the source (spent()), and the move is user-provided on purpose: that
// makes the type not trivially copyable, so std::bit_cast can't make one from bytes.
//
// Don't add a constructor, a friend, a factory or a cast. tests/compile_fail.cc checks that the ways around
// this don't compile, and scripts/check-invariants.sh checks for the rest.
class ApprovedOrder final {
 public:
  ApprovedOrder() = delete;
  ApprovedOrder(const ApprovedOrder&) = delete;
  ApprovedOrder& operator=(const ApprovedOrder&) = delete;
  ApprovedOrder(ApprovedOrder&& other) noexcept
      : order_(other.order_), stamp_(other.stamp_), spent_(other.spent_) {
    other.spend();
  }
  ApprovedOrder& operator=(ApprovedOrder&& other) noexcept {
    if (this != &other) {
      order_ = other.order_;
      stamp_ = other.stamp_;
      spent_ = other.spent_;
      other.spend();
    }
    return *this;
  }
  ~ApprovedOrder() = default;

  // Exactly what the gate evaluated.
  [[nodiscard]] const oms::OrderDetails& order() const noexcept { return order_; }
  [[nodiscard]] const GateStamp& stamp() const noexcept { return stamp_; }
  // Moved from: it approves nothing.
  [[nodiscard]] bool spent() const noexcept { return spent_; }

 private:
  friend class RiskGate;
  ApprovedOrder(const oms::OrderDetails& order, const GateStamp& stamp) noexcept
      : order_(order), stamp_(stamp) {}
  void spend() noexcept {
    order_ = {};
    stamp_ = {};
    spent_ = true;
  }

  oms::OrderDetails order_;
  GateStamp stamp_;
  bool spent_{false};
};

}  // namespace vyuha::risk

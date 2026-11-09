#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "vyuha/oms/oms.h"

namespace vyuha::oms {

// Splits an order at the exchange's freeze quantity. It is the only code that can mark orders as slices of
// one intent, which is what exempts them from R-DUP against each other (D-24). M2.5 adds the rest: lot
// rounding, ids for the children and gating the parent. Core thread only.
class FreezeSlicer final {
 public:
  FreezeSlicer() = delete;

  // Writes the children to `out` and returns how many, each at most max_child_qty, summing to parent.qty.
  // Every child has parent_id = parent.id and the same new intent. Returns 0, writing nothing, if the inputs
  // are bad or the children wouldn't fit.
  [[nodiscard]] static std::size_t slice(const OrderDetails& parent, std::int32_t max_child_qty,
                                         std::span<OrderDetails> out) noexcept {
    if (parent.qty <= 0 || max_child_qty <= 0) {
      return 0;
    }
    const std::size_t children =
        (static_cast<std::size_t>(parent.qty - 1) / static_cast<std::size_t>(max_child_qty)) + 1;
    if (children > out.size()) {
      return 0;
    }
    const SliceIntent intent(next_intent_++);
    std::int32_t remaining = parent.qty;
    for (std::size_t i = 0; i < children; ++i) {
      OrderDetails& child = out[i];
      child = parent;
      child.parent_id = parent.id;
      child.qty = std::min(remaining, max_child_qty);
      child.slice = intent;
      remaining -= child.qty;
    }
    return children;
  }

 private:
  // One counter for the whole engine, so no two slices can ever share an intent.
  inline static std::uint64_t next_intent_{1};
};

}  // namespace vyuha::oms

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "vyuha/oms/oms.h"

namespace vyuha::oms {

// Where the ids of sliced children come from. Injected so tests are deterministic; the production UUIDv7
// generator is wired in M2.8. Every call must return an id never returned before. Core thread.
class IOrderIdSource {
 public:
  IOrderIdSource() = default;
  IOrderIdSource(const IOrderIdSource&) = delete;
  IOrderIdSource& operator=(const IOrderIdSource&) = delete;
  IOrderIdSource(IOrderIdSource&&) = delete;
  IOrderIdSource& operator=(IOrderIdSource&&) = delete;
  virtual ~IOrderIdSource() = default;
  [[nodiscard]] virtual Uuid next() noexcept = 0;
};

// Splits an order at the exchange's freeze quantity (F-EX-3, F-RK-1). It is the only code that can mark
// orders as slices of one intent, which is what exempts them from R-DUP against each other (D-24). It only
// makes the children: SliceRunner gates and sends them. Core thread only.
class FreezeSlicer final {
 public:
  FreezeSlicer() = delete;

  // A child's client_order_id is the parent's plus "-NNN".
  static constexpr std::size_t kSuffixSize = 4;
  static constexpr std::size_t kMaxParentClientIdSize = FixedString<64>::capacity() - kSuffixSize;
  static constexpr std::size_t kMaxChildren = 999;

  // The largest child for an instrument (D-52): the largest whole number of lots strictly below the freeze
  // quantity, which is safe whether the instrument master holds the exchange's exact limit or the limit plus
  // one. So an order equal to the freeze quantity is sliced. 0 if there is none: freeze_qty or lot_size isn't
  // positive, or no whole lot fits below the freeze quantity (freeze_qty <= lot_size). The freeze quantity
  // comes only from the instrument master: the exchange revises it, so no value of it belongs in code. The
  // slicer cuts by this, and R-ORDER-SHAPE refuses a single F&O order above it (D-54): one function for both.
  [[nodiscard]] static constexpr std::int32_t largest_child(std::int32_t freeze_qty,
                                                            std::int32_t lot_size) noexcept {
    return freeze_qty <= 0 || lot_size <= 0 ? 0 : ((freeze_qty - 1) / lot_size) * lot_size;
  }

  // How many children a quantity needs. 0 for inputs that aren't positive.
  [[nodiscard]] static constexpr std::size_t child_count(std::int32_t qty,
                                                         std::int32_t max_child_qty) noexcept {
    return qty <= 0 || max_child_qty <= 0
               ? 0
               : (static_cast<std::size_t>(qty - 1) / static_cast<std::size_t>(max_child_qty)) + 1;
  }

  // Writes the children to `out` and returns how many, each at most max_child_qty, summing to parent.qty,
  // the remainder last. Each child is the parent with: its own id from `ids`, client_order_id
  // "<parent's>-001" and so on, the same new intent, and slice_parent = {parent.id, parent.qty, parent's
  // client_order_id}. parent_id stays as the parent had it: it is for brackets (D-48). For whole-lot children
  // the caller passes a max_child_qty that is a multiple of the lot (largest_child) and a parent that is one.
  //
  // Returns 0, writing nothing and drawing no id, if the inputs are bad: a qty or max_child_qty that isn't
  // positive, an empty client_order_id or one too long to suffix, an order that is already a slice, more than
  // kMaxChildren children, or more than fit in `out`. `parent` must not be an element of `out`.
  [[nodiscard]] static std::size_t slice(const OrderDetails& parent, std::int32_t max_child_qty,
                                         IOrderIdSource& ids, std::span<OrderDetails> out) noexcept {
    const std::size_t children = child_count(parent.qty, max_child_qty);
    const std::string_view base = parent.client_order_id.view();
    if (children == 0 || children > out.size() || children > kMaxChildren || base.empty() ||
        base.size() > kMaxParentClientIdSize || parent.slice.sliced() || parent.slice_parent.has_value()) {
      return 0;
    }
    const SliceIntent intent(next_intent_++);
    const SliceParent link{.id = parent.id, .qty = parent.qty, .client_order_id = parent.client_order_id};
    std::int32_t remaining = parent.qty;
    for (std::size_t i = 0; i < children; ++i) {
      OrderDetails& child = out[i];
      child = parent;
      child.id = ids.next();
      suffix(child.client_order_id, base, i + 1);
      child.qty = std::min(remaining, max_child_qty);
      child.slice = intent;
      child.slice_parent = link;
      remaining -= child.qty;
    }
    return children;
  }

  // "<base>-NNN" for child n, 1 to kMaxChildren. base must be at most kMaxParentClientIdSize long.
  static void suffix(FixedString<64>& out, std::string_view base, std::size_t n) noexcept {
    std::array<char, FixedString<64>::capacity()> text{};
    const std::size_t size = std::min(base.size(), kMaxParentClientIdSize);
    std::ranges::copy(base.substr(0, size), text.begin());
    const std::array<char, kSuffixSize> tail{'-', digit(n / 100), digit(n / 10), digit(n)};
    std::ranges::copy(tail, text.begin() + static_cast<std::ptrdiff_t>(size));
    static_cast<void>(out.assign({text.data(), size + kSuffixSize}));  // fits: size is bounded above
  }

 private:
  [[nodiscard]] static constexpr char digit(std::size_t n) noexcept {
    return static_cast<char>('0' + (n % 10));
  }

  // One counter for the whole engine, so no two slices can ever share an intent.
  inline static std::uint64_t next_intent_{1};
};

}  // namespace vyuha::oms

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/oms/oms.h"
#include "vyuha/risk/slot_index.h"
#include "vyuha/risk/wide.h"

namespace vyuha::risk {

// What the broker says an account can still use, and what one lot of an instrument needs (D-32). M3.5 keeps
// it current: the available figure every 10 s and after fills, the per-lot figures from the broker's margin
// API. Money is in Price units (a rupee is 10,000). Core thread only; nothing allocates after construction.
class MarginCache {
 public:
  struct Available {
    oms::Uuid account{};  // NOLINT(readability-redundant-member-init)
    std::int64_t amount{0};
    Wide reserved{0};  // taken by orders approved since `as_of`, so a burst can't spend the same money twice
    Nanos as_of{0};
  };
  struct PerLot {
    std::int64_t instrument_id{0};
    std::int64_t amount{0};
    Nanos as_of{0};
  };

  explicit MarginCache(std::size_t max_accounts = 64, std::size_t max_instruments = 16'384);

  // A fresh figure replaces the old one and clears what was reserved against it, so M3.5 must not refresh
  // with a figure that doesn't yet include the orders approved since it was asked for (D-39). False when the
  // table is full.
  bool set_available(const oms::Uuid& account, std::int64_t amount, Nanos as_of) noexcept;
  // False when the table is full, or the figure isn't above zero: a margin of nothing would pass every order.
  bool set_per_lot(std::int64_t instrument_id, std::int64_t amount, Nanos as_of) noexcept;
  // Takes `amount` off what the account can use until the next refresh.
  bool reserve(const oms::Uuid& account, Wide amount) noexcept;

  [[nodiscard]] const Available* available(const oms::Uuid& account) const noexcept;
  [[nodiscard]] const PerLot* per_lot(std::int64_t instrument_id) const noexcept;

 private:
  std::vector<Available> accounts_;
  std::size_t account_count_{0};
  std::vector<PerLot> lots_;
  std::size_t lot_count_{0};
  detail::SlotIndex lot_index_;
};

}  // namespace vyuha::risk

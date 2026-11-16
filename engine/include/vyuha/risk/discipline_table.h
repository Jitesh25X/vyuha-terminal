#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/oms/oms.h"

namespace vyuha::risk {

// Loss-streak lockouts (RISK_ENGINE §5), one per user for paper and one for live: the two keep separate
// streaks, a live lockout blocks live orders only, and paper losses never count toward the live streak
// (owner, 2026-11-15). The EOD job decides them from the days' P&L; the gate only reads them. `until` is the
// start of the last IST day the lockout covers. A user with no row isn't locked, but until the table says it
// is loaded the gate doesn't know that (D-39). Core thread only.
class DisciplineTable {
 public:
  // Room for a paper and a live lockout per user.
  explicit DisciplineTable(std::size_t max_users = 256) : capacity_(max_users * 2) {
    users_.reserve(capacity_);
  }

  void set_loaded(bool loaded) noexcept { loaded_ = loaded; }
  [[nodiscard]] bool loaded() const noexcept { return loaded_; }

  // False when the table is full.
  bool lock(const oms::Uuid& user, oms::PaperFlag mode, Nanos until) noexcept;
  void clear(const oms::Uuid& user, oms::PaperFlag mode) noexcept;
  [[nodiscard]] std::optional<Nanos> locked_until(const oms::Uuid& user, oms::PaperFlag mode) const noexcept;

 private:
  struct Row {
    oms::Uuid user{};  // NOLINT(readability-redundant-member-init)
    oms::PaperFlag mode{oms::kLiveOrder};
    Nanos until{0};
  };
  std::vector<Row> users_;
  std::size_t capacity_;  // reserve() may give more than asked, so the limit is kept here
  bool loaded_{false};
};

}  // namespace vyuha::risk

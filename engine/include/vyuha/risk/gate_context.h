#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "vyuha/core/clock.h"
#include "vyuha/core/pause_watch.h"
#include "vyuha/core/price.h"
#include "vyuha/md/id_map.h"
#include "vyuha/md/last_value.h"
#include "vyuha/md/session.h"
#include "vyuha/oms/oms.h"
#include "vyuha/oms/ops_window.h"
#include "vyuha/risk/kill_switch.h"

namespace vyuha::risk {

// What the rules read. The core thread owns it and is the only writer, and nothing here allocates after
// construction. Anything not loaded yet reads as "no": no account, no limit, no calendar. That refuses the
// order.

enum class Exchange : std::uint8_t { kNse, kBse, kNfo, kBfo };
enum class Segment : std::uint8_t { kEq, kFut, kOpt };

// OPS is counted per exchange, and NSE's cash and F&O segments are one exchange, as are BSE's. Counting them
// together can only be stricter than counting them apart.
inline constexpr std::size_t kOpsVenues = oms::kOpsVenues;
[[nodiscard]] constexpr std::uint8_t ops_venue(Exchange exchange) noexcept {
  return exchange == Exchange::kNse || exchange == Exchange::kNfo ? 0 : 1;
}

// NOLINTBEGIN(readability-redundant-member-init,cppcoreguidelines-pro-bounds-constant-array-index): {} keeps
// GCC quiet on designators, as for OrderDetails; each index is checked against the capacity just before it is
// used.

// A contract, from the instrument master. Index rows aren't loaded: they can't be traded.
struct InstrumentRef {
  std::int32_t lot_size{0};
  Price tick_size{};
  Exchange exchange{Exchange::kNse};
  Segment segment{Segment::kEq};
  Nanos expires_at{0};  // 15:30 IST on expiry day; 0 for a contract that doesn't expire
  bool fo_ban{false};
  bool restricted{false};
  // The instrument an option or a future is on; 0 for one that isn't on another (an equity is its own).
  std::int64_t underlying_id{0};
  // Today's circuit band, when known. A live order needs it (D-22); M2.3c supplies it from broker quotes.
  std::optional<Price> lower_circuit{};
  std::optional<Price> upper_circuit{};
};

class InstrumentTable {
 public:
  explicit InstrumentTable(std::size_t max_instruments) : map_(max_instruments) {}
  [[nodiscard]] bool put(std::int64_t id, const InstrumentRef& instrument) noexcept {
    InstrumentRef* slot = id > 0 ? map_.find_or_insert(static_cast<md::InstrumentId>(id)) : nullptr;
    if (slot == nullptr) {
      return false;
    }
    *slot = instrument;
    return true;
  }
  [[nodiscard]] const InstrumentRef* find(std::int64_t id) const noexcept {
    return id > 0 ? map_.find(static_cast<md::InstrumentId>(id)) : nullptr;
  }

 private:
  md::IdMap<InstrumentRef> map_;
};

struct AccountState {
  oms::Uuid id{};
  bool session_connected{false};
  Nanos token_expires_at{0};
  std::uint8_t ops_cap{0};  // broker_account.ops_cap
  std::array<oms::OpsWindow, kOpsVenues> ops{};
};

class AccountTable {
 public:
  static constexpr std::size_t kCapacity = 32;
  [[nodiscard]] AccountState* put(const AccountState& account) noexcept {
    if (AccountState* found = find(account.id); found != nullptr) {
      const auto ops = found->ops;  // a refresh must not hand back the orders-per-second budget
      *found = account;
      found->ops = ops;
      return found;
    }
    if (count_ == kCapacity) {
      return nullptr;
    }
    rows_[count_] = account;
    return &rows_[count_++];
  }
  [[nodiscard]] AccountState* find(const oms::Uuid& id) noexcept {
    const auto used = std::span(rows_).first(count_);
    const auto it = std::ranges::find(used, id, &AccountState::id);
    return it == used.end() ? nullptr : &*it;
  }
  [[nodiscard]] const AccountState* find(const oms::Uuid& id) const noexcept {
    const auto used = std::span(rows_).first(count_);
    const auto it = std::ranges::find(used, id, &AccountState::id);
    return it == used.end() ? nullptr : &*it;
  }

 private:
  std::array<AccountState, kCapacity> rows_{};
  std::size_t count_{0};
};

// account_user_map.can_trade, plus whether the user's role may trade live.
struct Entitlement {
  oms::Uuid user{};
  oms::Uuid account{};
  bool can_trade{false};
  bool can_live{false};
};

class EntitlementTable {
 public:
  static constexpr std::size_t kCapacity = 256;
  // Replaces the user's row for that account, so a revocation takes effect.
  [[nodiscard]] bool put(const Entitlement& row) noexcept {
    for (Entitlement& existing : std::span(rows_).first(count_)) {
      if (existing.user == row.user && existing.account == row.account) {
        existing = row;
        return true;
      }
    }
    if (count_ == kCapacity) {
      return false;
    }
    rows_[count_++] = row;
    return true;
  }
  [[nodiscard]] const Entitlement* find(const oms::Uuid& user, const oms::Uuid& account) const noexcept {
    for (const Entitlement& row : std::span(rows_).first(count_)) {
      if (row.user == user && row.account == account) {
        return &row;
      }
    }
    return nullptr;
  }

 private:
  std::array<Entitlement, kCapacity> rows_{};
  std::size_t count_{0};
};

// Strategies in state LIVE: a live order from any other strategy is refused.
class LiveStrategies {
 public:
  static constexpr std::size_t kCapacity = 128;
  void remove(const oms::Uuid& id) noexcept {
    const auto used = std::span(ids_).first(count_);
    const auto it = std::ranges::find(used, id);
    if (it != used.end()) {
      *it = used.back();
      --count_;
    }
  }
  [[nodiscard]] bool put(const oms::Uuid& id) noexcept {
    if (contains(id)) {
      return true;
    }
    if (count_ == kCapacity) {
      return false;
    }
    ids_[count_++] = id;
    return true;
  }
  [[nodiscard]] bool contains(const oms::Uuid& id) const noexcept {
    const auto used = std::span(ids_).first(count_);
    return std::ranges::find(used, id) != used.end();
  }

 private:
  std::array<oms::Uuid, kCapacity> ids_{};
  std::size_t count_{0};
};

// Keys of risk_limit that the M2.3 rules read; M2.4 adds its own. fat_finger_pct covers EQ and FUT, and
// fat_finger_pct_opt covers options, because the catalogue gives the two different defaults.
enum class LimitKey : std::uint8_t {
  kMaxOrderValue,
  kMaxOrderQtyLots,
  kOpsCap,
  kFatFingerPct,
  kFatFingerPctOpt,
  kDupWindowMs,
  kAllowMarketOrders,
  kAllowMisAfter,  // HHMM in IST
  kTradingWindowStart,
  kTradingWindowEnd,
  kMaxOpenOrders,
  kMaxPositionLots,
  kMaxPositionNotional,
  kMaxGrossExposure,
  kMaxNetDeltaInr,
  kMaxNetVegaInr,
  kMaxDailyLoss,
  kMaxDrawdownFromPeak,
};
inline constexpr std::size_t kLimitKeyCount = 18;

enum class LimitScope : std::uint8_t { kDesk, kAccount, kTrader, kStrategy, kInstrument };

// Which way is stricter for a key. A longest duplicate window is the strictest, as is a latest window start.
enum class Tightest : std::uint8_t { kSmallest, kLargest };

// value is risk_limit.value at numeric(20,4) scale, so 1 of anything is 10'000 and a rupee limit is in Price
// units.
inline constexpr std::int64_t kLimitScale = 10'000;

struct LimitRow {
  LimitScope scope{LimitScope::kDesk};
  oms::Uuid id{};                 // account, trader or strategy
  std::int64_t instrument_id{0};  // for LimitScope::kInstrument
  std::int64_t value{0};
  bool hard{true};
};

// The strictest applicable value of each kind (RISK_ENGINE §1). A hard limit rejects and a soft one warns.
struct ResolvedLimit {
  std::optional<std::int64_t> hard{};
  std::optional<std::int64_t> soft{};
  [[nodiscard]] bool any() const noexcept { return hard.has_value() || soft.has_value(); }
};

struct LimitSet {
  static constexpr std::size_t kRowsPerKey = 32;

  std::uint64_t version{0};
  std::array<std::array<LimitRow, kRowsPerKey>, kLimitKeyCount> rows{};
  std::array<std::uint8_t, kLimitKeyCount> counts{};

  [[nodiscard]] bool add(LimitKey key, const LimitRow& row) noexcept {
    const auto k = static_cast<std::size_t>(key);
    if (counts[k] == kRowsPerKey) {
      return false;
    }
    rows[k][counts[k]++] = row;
    return true;
  }

  // Whether a limit row covers this order: the desk's, its account's, trader's or strategy's, or its
  // instrument's.
  [[nodiscard]] static bool applies(const LimitRow& r, const oms::OrderDetails& order) noexcept {
    switch (r.scope) {
      case LimitScope::kDesk:
        return true;
      case LimitScope::kAccount:
        return r.id == order.account_id;
      case LimitScope::kTrader:
        return r.id == order.user_id;
      case LimitScope::kStrategy:
        return order.strategy_id.has_value() && r.id == *order.strategy_id;
      case LimitScope::kInstrument:
        return r.instrument_id == order.instrument_id;
    }
    return false;
  }

  // The strictest of every row that applies to this order.
  [[nodiscard]] ResolvedLimit resolve(LimitKey key, const oms::OrderDetails& order,
                                      Tightest tightest) const noexcept {
    return resolve_where(key, tightest, [&order](const LimitRow& r) noexcept { return applies(r, order); });
  }

  // Every row that applies, one at a time: a limit that is measured against a scope's own total (open orders,
  // exposure, P&L) has to be judged per row, since each scope has a different total.
  template <class Fn>
  void for_each_applicable(LimitKey key, const oms::OrderDetails& order, Fn fn) const noexcept {
    const auto k = static_cast<std::size_t>(key);
    for (const LimitRow& r : std::span(rows[k]).first(counts[k])) {
      if (applies(r, order)) {
        fn(r);
      }
    }
  }

  // Desk and account scopes only: an OPS bucket belongs to the account, not to a trader or strategy on it.
  [[nodiscard]] ResolvedLimit resolve_account(LimitKey key, const oms::Uuid& account,
                                              Tightest tightest) const noexcept {
    return resolve_where(key, tightest, [&account](const LimitRow& r) noexcept {
      return r.scope == LimitScope::kDesk || (r.scope == LimitScope::kAccount && r.id == account);
    });
  }

 private:
  template <class Applies>
  [[nodiscard]] ResolvedLimit resolve_where(LimitKey key, Tightest tightest, Applies applies) const noexcept {
    const auto k = static_cast<std::size_t>(key);
    ResolvedLimit out;
    const auto merge = [tightest](std::optional<std::int64_t>& slot, std::int64_t v) noexcept {
      if (!slot.has_value()) {
        slot = v;
      } else {
        slot = tightest == Tightest::kSmallest ? std::min(*slot, v) : std::max(*slot, v);
      }
    };
    for (const LimitRow& r : std::span(rows[k]).first(counts[k])) {
      if (applies(r)) {
        merge(r.hard ? out.hard : out.soft, r.value);
      }
    }
    return out;
  }
};

// Exchange days from exchange_holiday, plus how far the calendar has been loaded. Weekdays are regular
// sessions unless a row says otherwise, and a row with no minutes is a holiday.
struct CalendarDay {
  Exchange exchange{Exchange::kNse};
  Nanos day_start{0};  // ist_day_start of the day
  std::int32_t open_minute{0};
  std::int32_t close_minute{0};
};

struct SessionToday {
  bool known{false};
  bool open{false};
  bool special{false};
  std::int32_t open_minute{0};
  std::int32_t close_minute{0};
};

class TradingCalendar {
 public:
  static constexpr std::size_t kCapacity = 256;
  [[nodiscard]] bool add(const CalendarDay& day) noexcept {
    if (count_ == kCapacity) {
      return false;
    }
    days_[count_++] = day;
    return true;
  }
  void set_covered_until(Nanos day_start) noexcept { covered_until_ = day_start; }

  [[nodiscard]] SessionToday session(Exchange exchange, Nanos now) const noexcept {
    const Nanos day = md::ist_day_start(now);
    if (day > covered_until_) {
      return {};
    }
    for (const CalendarDay& d : std::span(days_).first(count_)) {
      if (d.day_start == day && d.exchange == exchange) {
        return {.known = true,
                .open = d.close_minute > d.open_minute,
                .special = true,
                .open_minute = d.open_minute,
                .close_minute = d.close_minute};
      }
    }
    const md::SessionHours hours;
    return {.known = true,
            .open = !weekend(day),
            .special = false,
            .open_minute = hours.open_minute,
            .close_minute = hours.close_minute};
  }

 private:
  [[nodiscard]] static bool weekend(Nanos ist_day_start) noexcept {
    constexpr Nanos kDay = Nanos{1'440} * kNanosPerMinute;
    constexpr Nanos kThursday = 4;  // 1970-01-01, with Sunday as 0
    const Nanos days = (ist_day_start + md::kIstOffset) / kDay;
    const Nanos weekday = (days + kThursday) % 7;
    return weekday == 0 || weekday == 6;
  }

  std::array<CalendarDay, kCapacity> days_{};
  std::size_t count_{0};
  Nanos covered_until_{INT64_MIN};
};

// Orders approved inside their duplicate window. Only approved orders go in, so R-OPS bounds how full it
// gets. Checked by a scan: at this size that costs less than a hash table's upkeep and needs no ordering by
// time.
struct DupEntry {
  oms::Uuid account{};
  std::int64_t instrument_id{0};
  oms::Side side{oms::Side::kBuy};
  std::int32_t qty{0};
  std::optional<Price> price{};
  std::optional<Price> trigger{};
  oms::SliceIntent slice{};
  Nanos ts{0};    // IClock::monotonic when approved (D-26)
  Nanos keep{0};  // how long this entry can still matter
};

struct DupTable {
  static constexpr std::size_t kCapacity = 1024;
  std::array<DupEntry, kCapacity> entries{};
  std::size_t count{0};
};

class ScopeBook;

// How old a Greek can be and still count (D-28). RISK_GREEKS_MAX_AGE_MS sets it.
inline constexpr Nanos kDefaultGreeksMaxAge = 15 * kNanosPerSecond;

struct GateContext {
  LimitSet limits{};
  KillSwitch kill{};
  bool live_trading_enabled{false};  // see live_trading_allowed
  const InstrumentTable* instruments{nullptr};
  const md::LastValueBook* book{nullptr};
  TradingCalendar calendar{};
  AccountTable accounts{};
  EntitlementTable entitlements{};
  LiveStrategies live_strategies{};
  DupTable dup{};
  // D-36. Data that arrived at or before this monotonic reading (LTPs; M2.4's Greeks and margin) is stale:
  // set when the gate sees a pause. Any order that prices off an LTP, reducing ones included, then waits for
  // a tick newer than the pause. Core thread only.
  PauseWatch pause{};
  Nanos stale_before{0};
  // Positions, working orders and what they add up to, per scope. Null reads as "positions not loaded"
  // (D-27).
  ScopeBook* scopes{nullptr};
  Nanos greeks_max_age{kDefaultGreeksMaxAge};
  [[nodiscard]] bool fresh(Nanos received_monotonic) const noexcept {
    return received_monotonic > stale_before;
  }
};

// NOLINTEND(readability-redundant-member-init,cppcoreguidelines-pro-bounds-constant-array-index)

}  // namespace vyuha::risk

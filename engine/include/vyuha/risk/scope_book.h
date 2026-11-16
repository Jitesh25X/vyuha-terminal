#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/core/price.h"
#include "vyuha/oms/oms.h"
#include "vyuha/risk/gate_context.h"
#include "vyuha/risk/slot_index.h"
#include "vyuha/risk/wide.h"

namespace vyuha::risk {

// NOLINTBEGIN(readability-redundant-member-init): the braces say what a fresh value is, as in oms.h

// Who a fill or a working order is counted against: the desk, its account, and its trader and strategy when
// it carries them. A fill that arrives only through reconciliation (an order placed in the broker's own app)
// has no trader or strategy, so it counts at account and desk scope only (D-35).
struct Attribution {
  oms::Uuid account{};
  std::optional<oms::Uuid> trader{};
  std::optional<oms::Uuid> strategy{};
};

struct Fill {
  Attribution who{};
  std::int64_t instrument_id{0};
  oms::Side side{oms::Side::kBuy};
  std::int64_t qty{0};  // units, always positive
  Price price{};
  std::int64_t cost{0};  // fees and taxes, in money units
};

// Money units per unit of the instrument: delta times the underlying's price, and vega per volatility point.
// Quant publishes them (D-28); `as_of` is IClock::monotonic when they arrived.
struct Greeks {
  std::int64_t delta{0};
  std::int64_t vega{0};
  Nanos as_of{0};
};

// One scope's net position in one instrument, with its working orders and what it adds to the scope's
// aggregates. The `c_` fields are what was last added, so an update is one subtraction and one addition.
struct Position {
  LimitScope kind{LimitScope::kDesk};
  oms::Uuid scope{};
  std::int64_t instrument_id{0};
  std::int64_t net_qty{0};  // units, signed
  Price avg_price{};        // of the open net position
  Wide realised{0};
  Wide costs{0};
  std::int64_t working_buy{0};  // quantity left on open buy orders
  std::int64_t working_sell{0};
  Wide c_notional{0};
  Wide c_delta{0};
  Wide c_vega{0};
  Wide c_unrealised{0};
  Wide c_short_lots{0};
  // Lots at the limit scale, worked out when the quantity changes so a check never divides.
  Wide net_lots{0};
  // What the instrument is, copied when the position is made: the cover check walks positions and mustn't
  // look each one up.
  Segment segment{Segment::kEq};
  OptionKind option_kind{OptionKind::kNone};
  Nanos expires_at{0};
  std::int32_t lot_size{0};
  bool no_mark{false};    // counted in positions_without_mark
  bool no_greeks{false};  // an option, counted in options_without_greeks
  bool held{false};       // a non-zero net, counted in its instrument's held_positions
  std::int32_t agg{-1};
  std::int32_t underlying{-1};
  std::int32_t next_in_instrument{-1};
  std::int32_t next_in_underlying{-1};
};

// What one scope adds up to, kept current as positions change, so a check never walks the positions.
struct ScopeAgg {
  LimitScope kind{LimitScope::kDesk};
  oms::Uuid id{};
  std::int64_t open_orders{0};
  Wide short_lots{0};          // short option lots held, at the limit scale
  Wide working_short_lots{0};  // option lots on open sell orders, at the limit scale
  Wide gross{0};               // sum of abs notional
  Wide delta{0};               // rupee delta
  Wide vega{0};
  Wide realised{0};
  Wide costs{0};
  Wide unrealised{0};
  Wide session_base{0};  // pnl() when the session began: "daily" measures what happened since (D-30)
  Wide peak_pnl{0};      // the best day P&L since the session began
  [[nodiscard]] Wide pnl() const noexcept { return realised + unrealised - costs; }
  [[nodiscard]] Wide day_pnl() const noexcept { return pnl() - session_base; }
};

// Positions, working orders and what they add up to, per scope (desk, account, trader, strategy), updated
// incrementally by fills, marks and Greeks (RISK_ENGINE §3 "GateContext holds pre-aggregated per-scope
// state"). M2.6 feeds it from the order log; nothing here reads Postgres. A table that fills up marks the
// book unhealthy, and the rules then refuse everything. Core thread only; nothing allocates after
// construction.
class ScopeBook {
 public:
  explicit ScopeBook(const InstrumentTable& instruments, std::size_t max_positions = 16'384);

  // Sources (D-27): until each is loaded the rules refuse the orders that need it.
  void set_positions_loaded(bool loaded) noexcept { positions_loaded_ = loaded; }
  void set_pnl_loaded(bool loaded) noexcept { pnl_loaded_ = loaded; }
  [[nodiscard]] bool positions_loaded() const noexcept { return positions_loaded_ && healthy_; }
  // Marks and costs for every position are known too: a position with no mark has no P&L.
  [[nodiscard]] bool pnl_loaded() const noexcept {
    return pnl_loaded_ && healthy_ && positions_without_mark_ == 0;
  }
  [[nodiscard]] bool healthy() const noexcept { return healthy_; }
  [[nodiscard]] std::int64_t positions_without_mark() const noexcept { return positions_without_mark_; }
  // An option held with no Greeks adds nothing to the aggregates, so delta and vega can't be trusted (D-28).
  [[nodiscard]] std::int64_t options_without_greeks() const noexcept { return options_without_greeks_; }

  // false when a table is full (the book is then unhealthy), or the instrument or fill is unusable.
  bool apply_fill(const Fill& fill) noexcept;
  // `as_of` is IClock::monotonic when the price arrived, as for an LTP (D-26): a mark has an age.
  bool set_mark(std::int64_t instrument_id, Price mark, Nanos as_of) noexcept;
  bool set_greeks(std::int64_t instrument_id, const Greeks& greeks) noexcept;
  // An order became (+) or stopped being (-) a working order for this much quantity.
  bool add_working(const Attribution& who, std::int64_t instrument_id, oms::Side side,
                   std::int64_t qty) noexcept;
  bool add_open_orders(const Attribution& who, std::int64_t delta) noexcept;
  // The start of a trading day: each scope's P&L for the day starts at nothing, and so does its peak.
  void begin_session() noexcept;
  // Whether every option held has Greeks that arrived after `stale_before`, and not more than `max_age` ago
  // by `monotonic`. A held option's Greeks are part of the totals, so a stale one makes them stale.
  [[nodiscard]] bool held_greeks_fresh(Nanos monotonic, Nanos max_age, Nanos stale_before) const noexcept;
  // The same for marks: whether every instrument held has a mark that arrived after `stale_before` and not
  // more than `max_age` ago. Exposure and P&L are worked out from the marks, so one old mark makes them old.
  [[nodiscard]] bool held_marks_fresh(Nanos monotonic, Nanos max_age, Nanos stale_before) const noexcept;

  [[nodiscard]] const Position* position(LimitScope kind, const oms::Uuid& scope,
                                         std::int64_t instrument_id) const noexcept;
  [[nodiscard]] const ScopeAgg* agg(LimitScope kind, const oms::Uuid& scope) const noexcept;
  [[nodiscard]] Wide underlying_notional(LimitScope kind, const oms::Uuid& scope,
                                         std::int64_t underlying_id) const noexcept;
  [[nodiscard]] const Greeks* greeks(std::int64_t instrument_id) const noexcept;

  // What an account holds on one underlying, for the short-option cover check (D-31). Everything is in
  // units of the underlying, never lots, so contracts with different lot sizes compare correctly (D-39 (11)).
  // `others_uncovered` is what the shorts of the other expiries leave over after their own longs, which they
  // would take from the futures first.
  struct CoverView {
    Wide long_units{0};        // long options of the kind and expiry asked for, any strike
    Wide short_units{0};       // short options of that group, with the working sells
    Wide future_units{0};      // futures that cover the kind, expiring at or after the option
    Wide others_uncovered{0};  // see above
    bool overflowed{false};    // more expiries than are tracked: the answer can't be trusted
  };
  [[nodiscard]] CoverView cover_view(const oms::Uuid& account, std::int64_t underlying_id, OptionKind kind,
                                     Nanos expiry) const noexcept;

  // Whether an order would take away cover that a short option of the account needs (D-39 (10)): a sell of a
  // long option, a sell of a long future (cover for calls) or a buy against a short future (cover for puts).
  // `need` is the short units that would be left wanting, `have` the futures left for them. An order that
  // leaves every short as covered as it was is not one, even if some short was uncovered already. More
  // expiries than are tracked, with a short held, counts as uncovering: the answer can't be trusted.
  struct Uncover {
    bool uncovers{false};
    Wide need{0};
    Wide have{0};
  };
  [[nodiscard]] Uncover uncover(const oms::Uuid& account, std::int64_t instrument_id, oms::Side side,
                                std::int64_t qty) const noexcept;

 private:
  struct InstState {
    std::int64_t instrument_id{0};
    Price mark{};
    bool has_mark{false};
    Nanos mark_as_of{0};
    bool option{false};  // set when a position in it is first held
    Greeks greeks{};
    bool has_greeks{false};
    std::int32_t head{-1};           // first position in this instrument
    std::int32_t held_slot{-1};      // its place in held_ while some scope holds it
    std::int32_t held_positions{0};  // how many scopes hold a non-zero net of it
  };
  struct UnderlyingAgg {
    LimitScope kind{LimitScope::kDesk};
    oms::Uuid scope{};
    std::int64_t underlying_id{0};
    Wide notional{0};
    std::int32_t head{-1};  // first position of this scope on the underlying
  };

  [[nodiscard]] std::int32_t find_position(LimitScope kind, const oms::Uuid& scope,
                                           std::int64_t instrument_id) const noexcept;
  [[nodiscard]] std::int32_t find_or_add_position(LimitScope kind, const oms::Uuid& scope,
                                                  std::int64_t instrument_id, std::int64_t underlying_id,
                                                  std::int32_t inst, const InstrumentRef& ref) noexcept;
  [[nodiscard]] std::int32_t find_agg(LimitScope kind, const oms::Uuid& scope) const noexcept;
  [[nodiscard]] std::int32_t find_or_add_agg(LimitScope kind, const oms::Uuid& scope) noexcept;
  [[nodiscard]] std::int32_t find_underlying(LimitScope kind, const oms::Uuid& scope,
                                             std::int64_t underlying_id) const noexcept;
  [[nodiscard]] std::int32_t find_or_add_underlying(LimitScope kind, const oms::Uuid& scope,
                                                    std::int64_t underlying_id) noexcept;
  [[nodiscard]] std::int32_t find_inst(std::int64_t instrument_id) const noexcept;
  [[nodiscard]] std::int32_t find_or_add_inst(std::int64_t instrument_id) noexcept;
  [[nodiscard]] std::int32_t find_or_add_inst_for_data(std::int64_t instrument_id) noexcept;
  void set_held(std::int32_t inst, bool held) noexcept;
  void refresh(std::int32_t position) noexcept;
  void fill_position(std::int32_t position, const Fill& fill) noexcept;

  const InstrumentTable* instruments_;
  bool positions_loaded_{false};
  bool pnl_loaded_{false};
  bool healthy_{true};
  std::int64_t positions_without_mark_{0};
  std::int64_t options_without_greeks_{0};

  std::vector<Position> positions_;
  std::size_t position_count_{0};
  detail::SlotIndex position_index_;
  std::vector<ScopeAgg> aggs_;
  std::size_t agg_count_{0};
  detail::SlotIndex agg_index_;
  std::vector<UnderlyingAgg> underlyings_;
  std::size_t underlying_count_{0};
  detail::SlotIndex underlying_index_;
  std::vector<InstState> insts_;
  std::size_t inst_count_{0};
  std::vector<std::int32_t> held_;  // the instruments with a non-zero position somewhere, as insts_ slots
  std::size_t held_count_{0};
  detail::SlotIndex inst_index_;
};

// NOLINTEND(readability-redundant-member-init)

}  // namespace vyuha::risk

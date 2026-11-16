// Rules 14 to 17, 19 and 20 of RISK_ENGINE §3 (M2.4a): the ones judged against a scope's own totals. 18, 21
// and 22 come in M2.4b. Each limit row is measured at its own scope: the desk's against the desk, an
// account's against that account, and so on.

#include <expected>
#include <optional>
#include <span>

#include "rule_support.h"
#include "vyuha/risk/rules.h"
#include "vyuha/risk/scope_book.h"
#include "vyuha/risk/wide.h"

namespace vyuha::risk {

namespace {

using detail::data_missing;
using detail::fresh_ltp;
using detail::reject;
using detail::warn;
using oms::OrderDetails;
using oms::Side;

// Where a limit row is measured. An instrument row has no scope of its own, so it is measured across the
// desk.
struct Measured {
  LimitScope kind{LimitScope::kDesk};
  oms::Uuid id{};
};

[[nodiscard]] Measured measured_at(const LimitRow& row, const OrderDetails& o) noexcept {
  switch (row.scope) {
    case LimitScope::kAccount:
      return {LimitScope::kAccount, o.account_id};
    case LimitScope::kTrader:
      return {LimitScope::kTrader, o.user_id};
    case LimitScope::kStrategy:
      return {LimitScope::kStrategy, o.strategy_id.value_or(oms::Uuid{})};  // applies() made sure it has one
    case LimitScope::kDesk:
    case LimitScope::kInstrument:
      break;
  }
  return {LimitScope::kDesk, {}};
}

// A measurement against one row, at the limit's scale (a count of 3 is 3 x kLimitScale).
struct Breach {
  Wide current{0};
  bool over{false};
};

// Judges every row that applies: the first hard breach rejects, else the first soft one warns. No row at all
// means no limit is loaded, and that refuses the order.
template <class Measure>
[[nodiscard]] Outcome judge_rows(const GateContext& ctx, const OrderDetails& o, LimitKey key,
                                 bool instrument_rows, Measure measure) noexcept {
  bool any = false;
  std::optional<Outcome> hard;
  std::optional<Outcome> soft;
  ctx.limits.for_each_applicable(key, o, [&](const LimitRow& row) noexcept {
    if (row.scope == LimitScope::kInstrument && !instrument_rows) {
      return;
    }
    any = true;
    const Breach b = measure(row, measured_at(row, o));
    if (!b.over) {
      return;
    }
    std::optional<Outcome>& slot = row.hard ? hard : soft;
    if (!slot.has_value()) {
      slot = row.hard ? reject(row.value, saturate(b.current)) : warn(row.value, saturate(b.current));
    }
  });
  if (!any) {
    return reject();
  }
  if (hard.has_value()) {
    return *hard;
  }
  return soft.value_or(Outcome{});
}

[[nodiscard]] Outcome worse(const Outcome& a, const Outcome& b) noexcept {
  if (a.verdict == Verdict::kReject) {
    return a;
  }
  if (b.verdict == Verdict::kReject) {
    return b;
  }
  return a.verdict == Verdict::kWarn ? a : b;
}

[[nodiscard]] bool positions_ready(const GateContext& ctx) noexcept {
  return ctx.scopes != nullptr && ctx.scopes->positions_loaded();
}

[[nodiscard]] Wide signed_added(const OrderDetails& o, const Position* p) noexcept {
  const bool buy = o.side == Side::kBuy;
  const Wide working = p == nullptr ? Wide{0} : Wide{buy ? p->working_buy : p->working_sell};
  const Wide added = working + Wide{o.qty};  // the same-side working orders on this instrument, and this one
  return buy ? added : -added;
}

[[nodiscard]] bool greeks_fresh(const GateContext& ctx, const Greeks& g, Nanos monotonic) noexcept {
  const Nanos age = monotonic - g.as_of;
  return ctx.fresh(g.as_of) && age >= 0 && age <= ctx.greeks_max_age;
}

// What one unit of the order's instrument counts for, in money units. A live order that needs Greeks it
// doesn't have is refused in `missing`; a paper order skips Greeks (D-27) and counts nothing for them.
struct OrderUnits {
  Wide notional{0};
  Wide delta{0};
  Wide vega{0};
  bool priced{false};
  std::optional<DataSource> missing;
};

[[nodiscard]] OrderUnits units_of(const GateContext& ctx, const OrderDetails& o,
                                  const GateFacts& f) noexcept {
  OrderUnits u;
  const bool option = f.instrument != nullptr && f.instrument->segment == Segment::kOpt;
  // The aggregates leave out an option held with no Greeks at all, so for a live order of any kind they can't
  // be trusted while one exists.
  if (o.is_paper.live() && ctx.scopes->options_without_greeks() > 0) {
    u.missing = DataSource::kGreeks;
  }
  if (option) {
    const Greeks* g = ctx.scopes->greeks(o.instrument_id);
    // A live option order also needs every option held to have fresh Greeks, since they are in the totals
    // (D-28).
    const bool fresh = g != nullptr && greeks_fresh(ctx, *g, f.monotonic) &&
                       ctx.scopes->held_greeks_fresh(f.monotonic, ctx.greeks_max_age, ctx.stale_before);
    if (fresh) {
      u.delta = g->delta;
      u.vega = g->vega;
      u.notional = wide_abs(Wide{g->delta});
    }
    if (o.is_paper.live() && !fresh) {
      u.missing = DataSource::kGreeks;
    }
    u.priced = true;
    return u;
  }
  // Valued at the worst of its price, its trigger and the LTP, as order_value does for a stop-market.
  std::optional<Price> unit;
  const auto raise = [&unit](const std::optional<Price>& candidate) noexcept {
    if (candidate.has_value() && (!unit.has_value() || *candidate > *unit)) {
      unit = candidate;
    }
  };
  raise(o.price);
  raise(o.trigger_price);
  raise(fresh_ltp(ctx, o.instrument_id, f.monotonic));
  if (unit.has_value()) {
    u.priced = true;
    u.notional = unit->units();
    u.delta = unit->units();  // delta 1: the rupee delta of a share or a future is its price
  }
  return u;
}

// ---- rule 14 ----

Outcome open_orders(const GateContext& ctx, const OrderDetails& o, const GateFacts& /*f*/) noexcept {
  if (!positions_ready(ctx)) {
    return data_missing(DataSource::kPositions);
  }
  return judge_rows(
      ctx, o, LimitKey::kMaxOpenOrders, false, [&ctx](const LimitRow& row, const Measured& at) noexcept {
        const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
        const Wide count = (agg != nullptr ? Wide{agg->open_orders} : Wide{0}) + 1;  // and this one
        return Breach{.current = count * kLimitScale, .over = count * kLimitScale > row.value};
      });
}

// ---- rule 15 ----

Outcome position(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!positions_ready(ctx)) {
    return data_missing(DataSource::kPositions);
  }
  if (f.instrument == nullptr || f.instrument->lot_size <= 0) {
    return reject();
  }
  const Wide lot = f.instrument->lot_size;
  const Outcome by_lots = judge_rows(
      ctx, o, LimitKey::kMaxPositionLots, true, [&](const LimitRow& row, const Measured& at) noexcept {
        const Position* p = ctx.scopes->position(at.kind, at.id, o.instrument_id);
        const Wide net = p != nullptr ? Wide{p->net_qty} : Wide{0};
        const Wide projected = wide_abs(net + signed_added(o, p));
        return Breach{.current = projected * kLimitScale / lot,
                      .over = projected * kLimitScale > row.value * lot};
      });
  if (by_lots.verdict == Verdict::kReject) {
    return by_lots;
  }
  const OrderUnits u = units_of(ctx, o, f);
  if (u.missing.has_value()) {
    return data_missing(*u.missing);
  }
  if (!u.priced) {
    return reject();
  }
  const std::int64_t underlying =
      f.instrument->underlying_id != 0 ? f.instrument->underlying_id : o.instrument_id;
  const Outcome by_notional = judge_rows(
      ctx, o, LimitKey::kMaxPositionNotional, false, [&](const LimitRow& row, const Measured& at) noexcept {
        const Position* p = ctx.scopes->position(at.kind, at.id, o.instrument_id);
        const Wide projected = ctx.scopes->underlying_notional(at.kind, at.id, underlying) +
                               (wide_abs(signed_added(o, p)) * u.notional);
        return Breach{.current = projected, .over = projected > row.value};
      });
  return worse(by_notional, by_lots);
}

// ---- rule 16 ----

Outcome exposure(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!positions_ready(ctx)) {
    return data_missing(DataSource::kPositions);
  }
  const OrderUnits u = units_of(ctx, o, f);
  if (u.missing.has_value()) {
    return data_missing(*u.missing);
  }
  if (!u.priced) {
    return reject();
  }
  return judge_rows(
      ctx, o, LimitKey::kMaxGrossExposure, false, [&](const LimitRow& row, const Measured& at) noexcept {
        const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
        const Position* p = ctx.scopes->position(at.kind, at.id, o.instrument_id);
        const Wide projected =
            (agg != nullptr ? agg->gross : Wide{0}) + (wide_abs(signed_added(o, p)) * u.notional);
        return Breach{.current = projected, .over = projected > row.value};
      });
}

// ---- rule 17 ----

Outcome greeks(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!positions_ready(ctx)) {
    return data_missing(DataSource::kPositions);
  }
  const OrderUnits u = units_of(ctx, o, f);
  if (u.missing.has_value()) {
    return data_missing(*u.missing);
  }
  if (!u.priced) {
    return reject();
  }
  const auto judge = [&](LimitKey key, bool vega) noexcept {
    return judge_rows(ctx, o, key, false, [&](const LimitRow& row, const Measured& at) noexcept {
      const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
      const Position* p = ctx.scopes->position(at.kind, at.id, o.instrument_id);
      Wide now = 0;
      if (agg != nullptr) {
        now = vega ? agg->vega : agg->delta;
      }
      const Wide per_unit = vega ? u.vega : u.delta;
      const Wide projected = wide_abs(now + (signed_added(o, p) * per_unit));
      return Breach{.current = projected, .over = projected > row.value};
    });
  };
  return worse(judge(LimitKey::kMaxNetDeltaInr, false), judge(LimitKey::kMaxNetVegaInr, true));
}

// ---- rules 19 and 20 ----

[[nodiscard]] bool pnl_ready(const GateContext& ctx) noexcept {
  return ctx.scopes != nullptr && ctx.scopes->pnl_loaded();
}

// D-30: the P&L as it stands, not including this order, which has none until it fills. A loss that has
// reached the limit blocks opening orders.
Outcome loss_daily(const GateContext& ctx, const OrderDetails& o, const GateFacts& /*f*/) noexcept {
  if (!pnl_ready(ctx)) {
    return data_missing(DataSource::kPnl);
  }
  return judge_rows(ctx, o, LimitKey::kMaxDailyLoss, false,
                    [&ctx](const LimitRow& row, const Measured& at) noexcept {
                      const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
                      const Wide loss = agg != nullptr ? -agg->day_pnl() : Wide{0};
                      return Breach{.current = loss < 0 ? Wide{0} : loss, .over = loss >= row.value};
                    });
}

Outcome drawdown(const GateContext& ctx, const OrderDetails& o, const GateFacts& /*f*/) noexcept {
  if (!pnl_ready(ctx)) {
    return data_missing(DataSource::kPnl);
  }
  return judge_rows(ctx, o, LimitKey::kMaxDrawdownFromPeak, false,
                    [&ctx](const LimitRow& row, const Measured& at) noexcept {
                      const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
                      const Wide down = agg != nullptr ? agg->peak_pnl - agg->day_pnl() : Wide{0};
                      return Breach{.current = down, .over = down >= row.value};
                    });
}

constexpr std::array<Rule, 6> kScopeRules{{
    {.code = RuleCode::kOpenOrders, .eval = &open_orders},
    {.code = RuleCode::kPosition, .eval = &position},
    {.code = RuleCode::kExposure, .eval = &exposure},
    {.code = RuleCode::kGreeks, .eval = &greeks},
    {.code = RuleCode::kLossDaily, .eval = &loss_daily},
    {.code = RuleCode::kDrawdown, .eval = &drawdown},
}};

}  // namespace

std::span<const Rule> scope_rules() noexcept { return kScopeRules; }

std::expected<Nanos, Error> parse_greeks_max_age_ms(std::string_view text) noexcept {
  constexpr std::string_view kMessage = "RISK_GREEKS_MAX_AGE_MS: whole milliseconds, 1 to 300000";
  constexpr std::int64_t kMaxMs = 300'000;
  if (text.empty()) {
    return kDefaultGreeksMaxAge;
  }
  std::int64_t ms = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, kMessage});
    }
    ms = (ms * 10) + (c - '0');
    if (ms > kMaxMs) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, kMessage});
    }
  }
  if (ms < 1) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, kMessage});
  }
  return ms * (kNanosPerSecond / 1'000);
}

}  // namespace vyuha::risk

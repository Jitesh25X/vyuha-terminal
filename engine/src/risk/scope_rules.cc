// Rules 14 to 22 of RISK_ENGINE §3 (M2.4a and M2.4b). 14 to 17, 19 and 20 are judged against a scope's own
// totals: each limit row is measured at its own scope, the desk's against the desk, an account's against
// that account, and so on. 18, 21 and 22 are short options, margin and discipline.

#include <expected>
#include <optional>
#include <span>

#include "rule_support.h"
#include "vyuha/md/session.h"
#include "vyuha/risk/discipline_table.h"
#include "vyuha/risk/margin_cache.h"
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

// What a live order needs of the marks (owner, 2026-11-16): its own instrument and its underlying, held or
// not, no older than an LTP may be (3 s); every other instrument held no older than ctx.mark_max_age_other.
// A mark from before a pause, or stamped in the future, is no mark at all. Refused R-DATA-MISSING:mark,
// naming the instrument in `current` and the age it was allowed, in ms, in `limit`. Only opening orders get
// here, so a reducing order still passes. A paper order goes on the marks there are.
[[nodiscard]] std::optional<Outcome> marks_missing(const GateContext& ctx, const OrderDetails& o,
                                                   const GateFacts& f) noexcept {
  if (!o.is_paper.live()) {
    return std::nullopt;
  }
  const std::int64_t underlying = f.instrument != nullptr ? f.instrument->underlying_id : 0;
  const auto stale = ctx.scopes->stale_mark(o.instrument_id, underlying,
                                            {.monotonic = f.monotonic,
                                             .stale_before = ctx.stale_before,
                                             .near_age = kMarkMaxAge,
                                             .other_age = ctx.mark_max_age_other});
  if (!stale.has_value()) {
    return std::nullopt;
  }
  Outcome out = data_missing(DataSource::kMark);
  out.current = stale->instrument_id;
  out.limit = stale->max_age / (kNanosPerSecond / 1'000);
  return out;
}

[[nodiscard]] bool greeks_fresh(const GateContext& ctx, const Greeks& g, Nanos monotonic) noexcept {
  const Nanos age = monotonic - g.as_of;
  return ctx.fresh(g.as_of) && age >= 0 && age <= ctx.greeks_max_age;
}

// What one unit of the order's instrument counts for, in money units. A live order that needs Greeks it
// doesn't have is refused in `missing`; a paper order skips Greeks (D-27) and counts nothing for them.
// The order valued at the worst of its price, its trigger and the LTP, as order_value does for a stop-market.
[[nodiscard]] std::optional<Price> worst_price(const GateContext& ctx, const OrderDetails& o,
                                               const GateFacts& f) noexcept {
  std::optional<Price> unit;
  const auto raise = [&unit](const std::optional<Price>& candidate) noexcept {
    if (candidate.has_value() && (!unit.has_value() || *candidate > *unit)) {
      unit = candidate;
    }
  };
  raise(o.price);
  raise(o.trigger_price);
  raise(fresh_ltp(ctx, o.instrument_id, f.monotonic));
  return unit;
}

struct OrderUnits {
  Wide notional{0};
  Wide delta{0};
  Wide vega{0};
  bool priced{false};
  std::optional<DataSource> missing;
  std::optional<Outcome> stale_mark;  // set with missing == kMark: the refusal that names the instrument
};

[[nodiscard]] OrderUnits units_of(const GateContext& ctx, const OrderDetails& o,
                                  const GateFacts& f) noexcept {
  OrderUnits u;
  const bool option = f.instrument != nullptr && f.instrument->segment == Segment::kOpt;
  if (const auto stale = marks_missing(ctx, o, f); stale.has_value()) {
    u.stale_mark = stale;
    u.missing = DataSource::kMark;
    return u;
  }
  // The aggregates leave out an option held with no Greeks at all, so for a live order of any kind they can't
  // be trusted while one exists (D-28; owner, 2026-11-15). Stale Greeks are another matter: only an option
  // order needs them fresh. A share or a future has delta 1 and no vega by definition, so its order reads no
  // Greeks.
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
  const std::optional<Price> unit = worst_price(ctx, o, f);
  if (unit.has_value()) {
    u.priced = true;
    u.notional = unit->units();
    u.delta = unit->units();  // delta 1: the rupee delta of a share or a future is its price
  }
  return u;
}

[[nodiscard]] Outcome missing_outcome(const OrderUnits& u) noexcept {
  if (u.stale_mark.has_value()) {
    return *u.stale_mark;
  }
  return data_missing(u.missing.value_or(DataSource::kNone));
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
    return missing_outcome(u);
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
    return missing_outcome(u);
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
    return missing_outcome(u);
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
Outcome loss_daily(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!pnl_ready(ctx)) {
    return data_missing(DataSource::kPnl);
  }
  if (const auto stale = marks_missing(ctx, o, f); stale.has_value()) {
    return *stale;
  }
  return judge_rows(ctx, o, LimitKey::kMaxDailyLoss, false,
                    [&ctx](const LimitRow& row, const Measured& at) noexcept {
                      const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
                      const Wide loss = agg != nullptr ? -agg->day_pnl() : Wide{0};
                      return Breach{.current = loss < 0 ? Wide{0} : loss, .over = loss >= row.value};
                    });
}

Outcome drawdown(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!pnl_ready(ctx)) {
    return data_missing(DataSource::kPnl);
  }
  if (const auto stale = marks_missing(ctx, o, f); stale.has_value()) {
    return *stale;
  }
  return judge_rows(ctx, o, LimitKey::kMaxDrawdownFromPeak, false,
                    [&ctx](const LimitRow& row, const Measured& at) noexcept {
                      const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
                      const Wide down = agg != nullptr ? agg->peak_pnl - agg->day_pnl() : Wide{0};
                      return Breach{.current = down, .over = down >= row.value};
                    });
}

// ---- rule 18: short options (D-31) ----

// The bool limit: allowed only if some hard row at the order's scopes says 1 and none says 0 (the strictest
// wins). No row at all means not allowed, the default.
[[nodiscard]] bool naked_allowed(const GateContext& ctx, const OrderDetails& o) noexcept {
  const ResolvedLimit limit = ctx.limits.resolve(LimitKey::kNakedShortOptions, o, Tightest::kSmallest);
  return limit.hard.has_value() && *limit.hard >= kLimitScale;
}

// An opening sell of an option against the lots limit: held shorts, working sells and this order.
Outcome short_lots_limit(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  const Wide lot = f.instrument->lot_size;
  if (lot <= 0) {
    return reject();
  }
  const Wide order_lots = (Wide{o.qty} * kLimitScale) / lot;
  return judge_rows(ctx, o, LimitKey::kMaxShortOptionLots, false,
                    [&](const LimitRow& row, const Measured& at) noexcept {
                      const ScopeAgg* agg = ctx.scopes->agg(at.kind, at.id);
                      const Wide held = agg != nullptr ? agg->short_lots + agg->working_short_lots : Wide{0};
                      const Wide projected = held + order_lots;  // working sells count against the limit
                      return Breach{.current = projected, .over = projected > row.value};
                    });
}

// An order that could take cover from a short: refused if it would leave more short units bare than there
// were.
Outcome uncover_check(const GateContext& ctx, const OrderDetails& o) noexcept {
  const ScopeBook::Uncover left = ctx.scopes->uncover(o.account_id, o.instrument_id, o.side, o.qty);
  if (!left.uncovers) {
    return {};
  }
  Outcome out = reject(saturate(left.have), saturate(left.need));
  out.reason = RuleReason::kUncover;
  return out;
}

// Cover is compared in units of the underlying, not lots, so it holds whatever the contracts' lot sizes are,
// a revised lot size included (D-39 (11)).
Outcome cover_check(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  const Wide order_units = o.qty;
  const std::int64_t underlying = f.instrument->underlying_id;
  const OptionKind kind = f.instrument->option_kind;
  const auto no_cover = [](Wide have, Wide need) noexcept {
    Outcome out = reject(saturate(have), saturate(need));
    out.reason = RuleReason::kNoCover;
    return out;
  };
  if (underlying == 0 || kind == OptionKind::kNone) {
    return no_cover(0, order_units);  // can't tell what would cover it
  }
  const ScopeBook::CoverView view =
      ctx.scopes->cover_view(o.account_id, underlying, kind, f.instrument->expires_at);
  if (view.overflowed) {
    return no_cover(0, order_units);
  }
  const Wide need = view.short_units + order_units;
  // Futures cover shorts unit for unit, and not twice: shorts of other expiries that their own longs don't
  // cover take them first, whichever of them the futures could really serve. An order that sells through a
  // long of its own (a flip) is counted the same way: the long it uses up is in `have`, and the whole order
  // is in `need`.
  const Wide pool = view.future_units - view.others_uncovered;
  const Wide have = view.long_units + (pool > 0 ? pool : Wide{0});
  return need > have ? no_cover(have, need) : Outcome{};
}

// The rule runs for every order, not only opening ones (D-39 (10)): an order that takes cover away from a
// short is judged as an opening one would be, though it reduces a position. Three cases. An opening sell of
// an option must fit the lots limit and have cover. Any other order that reduces a long option or a covering
// future must leave every short as covered as it was. Everything else passes.
Outcome short_opt(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (f.instrument == nullptr) {
    return reject();  // can't tell what it is, so can't tell whether it needs or takes cover: fail closed
  }
  const Segment segment = f.instrument->segment;
  const bool sell = o.side == Side::kSell;
  const bool opening_short = f.opening && segment == Segment::kOpt && sell;
  // A sold option or a future traded either way can take cover away; nothing else can.
  const bool may_uncover = !opening_short && ((segment == Segment::kOpt && sell) || segment == Segment::kFut);
  if (!opening_short && !may_uncover) {
    return {};
  }
  if (!positions_ready(ctx)) {
    return data_missing(DataSource::kPositions);
  }
  Outcome by_limit;
  if (opening_short) {
    by_limit = short_lots_limit(ctx, o, f);
    if (by_limit.verdict == Verdict::kReject) {
      return by_limit;
    }
  }
  if (naked_allowed(ctx, o)) {
    return by_limit;
  }
  if (may_uncover) {
    return uncover_check(ctx, o);
  }
  const Outcome covered = cover_check(ctx, o, f);
  return covered.verdict == Verdict::kReject ? covered : by_limit;
}

// ---- rule 21: margin (D-32) ----

struct Required {
  Wide amount{0};  // money units, before the buffer
  bool known{false};
};

// What the order needs. A bought option costs its premium; a future or a sold option needs the broker's
// margin per lot; shares are taken at their value, an upper bound (a reading, D-39).
[[nodiscard]] Required required_margin(const GateContext& ctx, const OrderDetails& o,
                                       const GateFacts& f) noexcept {
  if (f.instrument == nullptr || f.instrument->lot_size <= 0) {
    return {};
  }
  const InstrumentRef& i = *f.instrument;
  const bool by_lot = i.segment == Segment::kFut || (i.segment == Segment::kOpt && o.side == Side::kSell);
  if (by_lot) {
    const MarginCache::PerLot* per = ctx.margin->per_lot(o.instrument_id);
    if (per == nullptr || per->amount <= 0 || !ctx.fresh(per->as_of)) {
      return {};
    }
    const Nanos age = f.monotonic - per->as_of;
    if (age < 0 || age > ctx.margin_max_age) {
      return {};
    }
    const Wide lot = i.lot_size;
    return {.amount = ((Wide{per->amount} * Wide{o.qty}) + lot - 1) / lot, .known = true};  // rounded up
  }
  const std::optional<Price> unit = worst_price(ctx, o, f);
  if (!unit.has_value()) {
    return {};
  }
  return {.amount = Wide{o.qty} * unit->units(), .known = true};
}

constexpr Wide kPercent = 100;

// The money an order has to have free, with the buffer on top, rounded up.
[[nodiscard]] Wide buffered(Wide required, std::int64_t buffer) noexcept {
  const Wide scale = kPercent * kLimitScale;
  return ((required * (scale + buffer)) + scale - 1) / scale;
}

// A live order over this much goes through the broker's margin preview first (RISK_ENGINE §3). That preview
// is M3.5's, so until then such an order is refused (D-39).
constexpr Wide kPreviewAbove = Wide{1'000'000} * Price::kUnitsPerRupee;

Outcome margin(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!o.is_paper.live()) {
    return {};  // paper orders skip margin (D-27)
  }
  if (ctx.margin == nullptr) {
    return data_missing(DataSource::kMargin);
  }
  const MarginCache::Available* have = ctx.margin->available(o.account_id);
  if (have == nullptr || !ctx.fresh(have->as_of) || f.monotonic - have->as_of < 0 ||
      f.monotonic - have->as_of > ctx.margin_max_age) {
    return data_missing(DataSource::kMargin);
  }
  const Required need = required_margin(ctx, o, f);
  if (!need.known) {
    return data_missing(DataSource::kMargin);
  }
  const std::optional<Price> unit = worst_price(ctx, o, f);
  if (!unit.has_value() || (Wide{o.qty} * unit->units()) > kPreviewAbove) {
    return data_missing(DataSource::kMargin);
  }
  const ResolvedLimit buffer = ctx.limits.resolve(LimitKey::kMarginBufferPct, o, Tightest::kLargest);
  if (!buffer.hard.has_value() || *buffer.hard < 0) {
    return reject();  // no buffer is loaded, or a negative one that would loosen the check
  }
  const Wide total = buffered(need.amount, *buffer.hard);
  const Wide free = Wide{have->amount} - have->reserved;
  if (total > free) {
    return reject(saturate(free), saturate(total));
  }
  return {};
}

// What an approved order takes off the account's free margin until the next refresh, so a burst can't spend
// the same money twice. Called only when the whole chain passed.
void margin_commit(GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!o.is_paper.live() || ctx.margin == nullptr) {
    return;
  }
  const Required need = required_margin(ctx, o, f);
  const ResolvedLimit buffer = ctx.limits.resolve(LimitKey::kMarginBufferPct, o, Tightest::kLargest);
  if (need.known && buffer.hard.has_value() && *buffer.hard >= 0) {  // check() required all three
    static_cast<void>(ctx.margin->reserve(o.account_id, buffered(need.amount, *buffer.hard)));
  }
}

// ---- rule 22: discipline ----

Outcome discipline(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (ctx.discipline == nullptr || !ctx.discipline->loaded()) {
    // A paper order can't lose money, so not knowing the lockouts doesn't stop it.
    return o.is_paper.live() ? data_missing(DataSource::kDiscipline) : Outcome{};
  }
  // Paper and live keep separate streaks and separate lockouts (owner, 2026-11-15): a live lockout blocks
  // live orders only, and a paper one paper orders only.
  const std::optional<Nanos> until = ctx.discipline->locked_until(o.user_id, o.is_paper);
  const Nanos today = md::ist_day_start(f.now);
  if (until.has_value() && today <= *until) {
    return reject(*until, today);
  }
  return {};
}

constexpr std::array<Rule, 3> kLateRules{{
    {.code = RuleCode::kShortOpt, .eval = &short_opt},
    {.code = RuleCode::kMargin, .eval = &margin, .on_approved = &margin_commit},
    {.code = RuleCode::kDiscipline, .eval = &discipline},
}};

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

std::span<const Rule> late_rules() noexcept { return kLateRules; }

std::span<const Rule> all_rules() noexcept {
  // The 22 rules in the order of RISK_ENGINE §3, put together once.
  static const std::array<Rule, kRuleCount> kAll = [] {
    std::array<Rule, kRuleCount> table{};
    for (const auto part : {pre_trade_rules(), scope_rules(), late_rules()}) {
      for (const Rule& r : part) {
        table.at(static_cast<std::size_t>(r.code) - 1) = r;
      }
    }
    return table;
  }();
  return kAll;
}

namespace {

std::expected<Nanos, Error> parse_age_ms(std::string_view text, Nanos fallback, std::int64_t max_ms,
                                         std::string_view message) noexcept {
  if (text.empty()) {
    return fallback;
  }
  std::int64_t ms = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, message});
    }
    ms = (ms * 10) + (c - '0');
    if (ms > max_ms) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, message});
    }
  }
  if (ms < 1) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, message});
  }
  return ms * (kNanosPerSecond / 1'000);
}

}  // namespace

std::expected<Nanos, Error> parse_greeks_max_age_ms(std::string_view text) noexcept {
  return parse_age_ms(text, kDefaultGreeksMaxAge, 300'000,
                      "RISK_GREEKS_MAX_AGE_MS: whole milliseconds, 1 to 300000");
}

std::expected<Nanos, Error> parse_margin_max_age_ms(std::string_view text) noexcept {
  return parse_age_ms(text, kDefaultMarginMaxAge, 300'000,
                      "RISK_MARGIN_MAX_AGE_MS: whole milliseconds, 1 to 300000");
}

std::expected<Nanos, Error> parse_mark_max_age_other_ms(std::string_view text) noexcept {
  return parse_age_ms(text, kDefaultMarkMaxAgeOther, 300'000,
                      "RISK_MARK_MAX_AGE_OTHER_MS: whole milliseconds, 1 to 300000");
}

}  // namespace vyuha::risk

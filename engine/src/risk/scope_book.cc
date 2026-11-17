#include "vyuha/risk/scope_book.h"

#include <algorithm>
#include <span>

namespace vyuha::risk {

namespace {

using oms::Side;
using oms::Uuid;

constexpr std::size_t kMaxScopes = 512;
constexpr std::size_t kMaxInstruments = 8'192;

[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t x) noexcept {
  x ^= x >> 30U;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27U;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31U;
  return x;
}

[[nodiscard]] std::uint64_t hash_uuid(const Uuid& id) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const auto byte : id.bytes) {
    h = (h ^ byte) * 0x100000001b3ULL;
  }
  return h;
}

[[nodiscard]] std::uint64_t hash_key(LimitScope kind, const Uuid& id, std::int64_t extra) noexcept {
  return mix(hash_uuid(id) ^ mix(static_cast<std::uint64_t>(extra)) ^
             (static_cast<std::uint64_t>(kind) * 0x9e3779b97f4a7c15ULL));
}

// Units as lots at the limit scale; a contract with no lot size has none, which the rules refuse.
[[nodiscard]] Wide lots_at_limit_scale(Wide units, std::int32_t lot_size) noexcept {
  if (lot_size <= 0) {
    return 0;
  }
  return lot_size == 1 ? units * kLimitScale : (units * kLimitScale) / Wide{lot_size};
}

template <class V>
[[nodiscard]] auto& at(V& v, std::int32_t i) noexcept {
  return v[static_cast<std::size_t>(i)];
}

struct ScopeRef {
  LimitScope kind{LimitScope::kDesk};
  Uuid id{};
};
struct ScopeList {
  std::array<ScopeRef, 4> items{};
  std::size_t count{0};
};

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): at most four, and the count is checked
[[nodiscard]] ScopeList scopes_of(const Attribution& who) noexcept {
  ScopeList list;
  list.items[list.count++] = {LimitScope::kDesk, Uuid{}};
  list.items[list.count++] = {LimitScope::kAccount, who.account};
  if (who.trader.has_value()) {
    list.items[list.count++] = {LimitScope::kTrader, *who.trader};
  }
  if (who.strategy.has_value()) {
    list.items[list.count++] = {LimitScope::kStrategy, *who.strategy};
  }
  return list;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

}  // namespace

ScopeBook::ScopeBook(const InstrumentTable& instruments, std::size_t max_positions)
    : instruments_(&instruments),
      positions_(max_positions),
      position_index_(max_positions),
      aggs_(kMaxScopes),
      agg_index_(kMaxScopes),
      underlyings_(max_positions),
      underlying_index_(max_positions),
      insts_(kMaxInstruments),
      held_(kMaxInstruments),
      inst_index_(kMaxInstruments) {}

std::int32_t ScopeBook::find_position(LimitScope kind, const Uuid& scope,
                                      std::int64_t instrument_id) const noexcept {
  return position_index_.find(hash_key(kind, scope, instrument_id), [&](std::int32_t s) noexcept {
    const Position& p = positions_[static_cast<std::size_t>(s)];
    return p.kind == kind && p.instrument_id == instrument_id && p.scope == scope;
  });
}

std::int32_t ScopeBook::find_agg(LimitScope kind, const Uuid& scope) const noexcept {
  return agg_index_.find(hash_key(kind, scope, 0), [&](std::int32_t s) noexcept {
    const ScopeAgg& a = aggs_[static_cast<std::size_t>(s)];
    return a.kind == kind && a.id == scope;
  });
}

std::int32_t ScopeBook::find_or_add_agg(LimitScope kind, const Uuid& scope) noexcept {
  if (const std::int32_t found = find_agg(kind, scope); found >= 0) {
    return found;
  }
  if (agg_count_ == aggs_.size()) {
    return -1;
  }
  const auto slot = static_cast<std::int32_t>(agg_count_++);
  at(aggs_, slot) = ScopeAgg{.kind = kind, .id = scope};
  agg_index_.insert(hash_key(kind, scope, 0), slot);
  return slot;
}

std::int32_t ScopeBook::find_underlying(LimitScope kind, const Uuid& scope,
                                        std::int64_t underlying_id) const noexcept {
  return underlying_index_.find(hash_key(kind, scope, underlying_id), [&](std::int32_t s) noexcept {
    const UnderlyingAgg& u = underlyings_[static_cast<std::size_t>(s)];
    return u.kind == kind && u.underlying_id == underlying_id && u.scope == scope;
  });
}

std::int32_t ScopeBook::find_or_add_underlying(LimitScope kind, const Uuid& scope,
                                               std::int64_t underlying_id) noexcept {
  if (const std::int32_t found = find_underlying(kind, scope, underlying_id); found >= 0) {
    return found;
  }
  if (underlying_count_ == underlyings_.size()) {
    return -1;
  }
  const auto slot = static_cast<std::int32_t>(underlying_count_++);
  at(underlyings_, slot) = UnderlyingAgg{.kind = kind, .scope = scope, .underlying_id = underlying_id};
  underlying_index_.insert(hash_key(kind, scope, underlying_id), slot);
  return slot;
}

std::int32_t ScopeBook::find_inst(std::int64_t instrument_id) const noexcept {
  return inst_index_.find(mix(static_cast<std::uint64_t>(instrument_id)), [&](std::int32_t s) noexcept {
    return insts_[static_cast<std::size_t>(s)].instrument_id == instrument_id;
  });
}

std::int32_t ScopeBook::find_or_add_inst(std::int64_t instrument_id) noexcept {
  if (const std::int32_t found = find_inst(instrument_id); found >= 0) {
    return found;
  }
  if (inst_count_ == insts_.size()) {
    return -1;
  }
  const auto slot = static_cast<std::int32_t>(inst_count_++);
  at(insts_, slot) = InstState{.instrument_id = instrument_id};
  inst_index_.insert(mix(static_cast<std::uint64_t>(instrument_id)), slot);
  return slot;
}

// A mark or Greeks for an instrument nobody holds is only worth keeping while there is room: half the table
// is kept for the instruments with positions or working orders, so a feed that ticks everything can't crowd
// them out and halt the desk.
std::int32_t ScopeBook::find_or_add_inst_for_data(std::int64_t instrument_id) noexcept {
  if (const std::int32_t found = find_inst(instrument_id); found >= 0) {
    return found;
  }
  return inst_count_ >= insts_.size() / 2 ? -1 : find_or_add_inst(instrument_id);
}

void ScopeBook::set_held(std::int32_t inst, bool held) noexcept {
  InstState& state = at(insts_, inst);
  if (held && state.held_slot < 0) {
    state.held_slot = static_cast<std::int32_t>(held_count_);
    at(held_, state.held_slot) = inst;
    ++held_count_;
  } else if (!held && state.held_slot >= 0) {
    const std::int32_t slot = state.held_slot;
    const std::int32_t last = at(held_, static_cast<std::int32_t>(held_count_ - 1));
    at(held_, slot) = last;
    at(insts_, last).held_slot = slot;
    --held_count_;
    state.held_slot = -1;
  }
}

std::int32_t ScopeBook::find_or_add_position(LimitScope kind, const Uuid& scope, std::int64_t instrument_id,
                                             std::int64_t underlying_id, std::int32_t inst,
                                             const InstrumentRef& ref) noexcept {
  if (const std::int32_t found = find_position(kind, scope, instrument_id); found >= 0) {
    return found;
  }
  if (position_count_ == positions_.size()) {
    return -1;
  }
  const std::int32_t agg = find_or_add_agg(kind, scope);
  const std::int32_t under = find_or_add_underlying(kind, scope, underlying_id);
  if (agg < 0 || under < 0) {
    return -1;
  }
  const auto slot = static_cast<std::int32_t>(position_count_++);
  InstState& state = at(insts_, inst);
  UnderlyingAgg& under_agg = at(underlyings_, under);
  at(positions_, slot) = Position{.kind = kind,
                                  .scope = scope,
                                  .instrument_id = instrument_id,
                                  .segment = ref.segment,
                                  .option_kind = ref.option_kind,
                                  .expires_at = ref.expires_at,
                                  .lot_size = ref.lot_size,
                                  .agg = agg,
                                  .underlying = under,
                                  .next_in_instrument = state.head,
                                  .next_in_underlying = under_agg.head};
  state.head = slot;
  under_agg.head = slot;
  position_index_.insert(hash_key(kind, scope, instrument_id), slot);
  return slot;
}

// Recomputes what a position adds to its scope, from its net quantity, the mark and the Greeks, and moves the
// scope's aggregates by the difference.
void ScopeBook::refresh(std::int32_t position) noexcept {
  Position& p = at(positions_, position);
  const std::int32_t slot = find_inst(p.instrument_id);
  const InstState* inst = slot >= 0 ? &at(insts_, slot) : nullptr;
  const bool has_mark = inst != nullptr && inst->has_mark;
  const Wide mark = has_mark ? Wide{inst->mark.units()} : Wide{0};
  const bool option = p.segment == Segment::kOpt;  // as it was when the position was made, not the master now
  const bool has_greeks = option && inst != nullptr && inst->has_greeks;

  Wide unit_notional = 0;
  Wide unit_delta = 0;
  Wide unit_vega = 0;
  if (!option) {
    unit_notional = mark;  // equities and futures: delta 1, so rupee delta is the price
    unit_delta = mark;
  } else if (has_greeks) {
    unit_delta = inst->greeks.delta;
    unit_vega = inst->greeks.vega;
    unit_notional = wide_abs(unit_delta);  // delta-notional (D-35)
  }
  const Wide net = p.net_qty;
  const Wide notional = wide_abs(net) * unit_notional;
  const Wide delta = net * unit_delta;
  const Wide vega = net * unit_vega;
  const Wide unrealised = has_mark ? net * (mark - p.avg_price.units()) : Wide{0};

  ScopeAgg& agg = at(aggs_, p.agg);
  // Short option lots at the limit scale; a contract with no lot size can't be counted, which the rules
  // refuse.
  const Wide short_lots = (option && net < 0) ? -p.net_lots : Wide{0};
  agg.short_lots += short_lots - p.c_short_lots;
  p.c_short_lots = short_lots;
  agg.gross += notional - p.c_notional;
  agg.delta += delta - p.c_delta;
  agg.vega += vega - p.c_vega;
  agg.unrealised += unrealised - p.c_unrealised;
  at(underlyings_, p.underlying).notional += notional - p.c_notional;
  p.c_notional = notional;
  p.c_delta = delta;
  p.c_vega = vega;
  p.c_unrealised = unrealised;
  agg.peak_pnl = std::max(agg.peak_pnl, agg.day_pnl());
  // An instrument is held while any scope holds it: the desk can be flat while an account and another carry
  // offsetting positions, and each of them still has its mark, delta and vega in its totals.
  if (slot >= 0 && (net != 0) != p.held) {
    InstState& state = at(insts_, slot);
    state.option = option;
    p.held = net != 0;
    state.held_positions += p.held ? 1 : -1;
    set_held(slot, state.held_positions > 0);
  }

  const bool no_mark = net != 0 && !has_mark;
  positions_without_mark_ += static_cast<std::int64_t>(no_mark) - static_cast<std::int64_t>(p.no_mark);
  p.no_mark = no_mark;
  const bool no_greeks = net != 0 && option && !has_greeks;
  options_without_greeks_ += static_cast<std::int64_t>(no_greeks) - static_cast<std::int64_t>(p.no_greeks);
  p.no_greeks = no_greeks;
}

void ScopeBook::fill_position(std::int32_t position, const Fill& fill) noexcept {
  Position& p = at(positions_, position);
  ScopeAgg& agg = at(aggs_, p.agg);
  const Wide qty = fill.qty;
  const Wide signed_qty = fill.side == Side::kBuy ? qty : -qty;
  const Wide net = p.net_qty;
  Wide realised = 0;
  if (net == 0 || (net > 0) == (signed_qty > 0)) {
    const Wide held = wide_abs(net);
    p.avg_price = Price::from_units(
        saturate(((held * p.avg_price.units()) + (qty * fill.price.units())) / (held + qty)));
    p.net_qty = saturate(net + signed_qty);
    if (Wide{p.net_qty} != net + signed_qty) {
      healthy_ = false;  // clipped: the position is bigger than we can count
    }
  } else {
    const Wide closing = std::min(wide_abs(net), qty);
    // A long gains when it sells above its average, a short when it buys below it.
    realised = closing * (net > 0 ? Wide{fill.price.units()} - p.avg_price.units()
                                  : Wide{p.avg_price.units()} - fill.price.units());
    p.net_qty = saturate(net + signed_qty);
    if (p.net_qty == 0) {
      p.avg_price = Price{};
    } else if (qty > closing) {
      p.avg_price = fill.price;  // it flipped: the rest opens at the fill price
    }
  }
  p.net_lots = lots_at_limit_scale(Wide{p.net_qty}, p.lot_size);
  p.realised += realised;
  p.costs += fill.cost;
  agg.realised += realised;
  agg.costs += fill.cost;
  refresh(position);
}

bool ScopeBook::apply_fill(const Fill& fill) noexcept {
  if (fill.qty <= 0 || fill.price <= Price{}) {
    return false;
  }
  const InstrumentRef* ref = instruments_->find(fill.instrument_id);
  if (ref == nullptr) {
    return false;
  }
  if (ref->segment != Segment::kEq && ref->lot_size <= 0) {
    healthy_ = false;  // a future or option with no lot size can't be counted in lots
    return false;
  }
  const std::int64_t underlying = ref->underlying_id != 0 ? ref->underlying_id : fill.instrument_id;
  const std::int32_t inst = find_or_add_inst(fill.instrument_id);
  if (inst < 0) {
    healthy_ = false;
    return false;
  }
  const ScopeList list = scopes_of(fill.who);
  for (std::size_t i = 0; i < list.count; ++i) {
    const ScopeRef& scope = list.items[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    const std::int32_t p =
        find_or_add_position(scope.kind, scope.id, fill.instrument_id, underlying, inst, *ref);
    if (p < 0) {
      healthy_ = false;
      return false;
    }
    fill_position(p, fill);
  }
  return true;
}

bool ScopeBook::set_mark(std::int64_t instrument_id, Price mark, Nanos as_of) noexcept {
  const std::int32_t slot = find_or_add_inst_for_data(instrument_id);
  if (slot < 0) {
    return false;  // nobody holds it and the table is half full: not worth a slot, and not an error
  }
  InstState& inst = at(insts_, slot);
  inst.mark = mark;
  inst.has_mark = mark > Price{};
  inst.mark_as_of = as_of;
  for (std::int32_t p = inst.head; p >= 0; p = at(positions_, p).next_in_instrument) {
    refresh(p);
  }
  return true;
}

bool ScopeBook::set_greeks(std::int64_t instrument_id, const Greeks& greeks) noexcept {
  const std::int32_t slot = find_or_add_inst_for_data(instrument_id);
  if (slot < 0) {
    return false;
  }
  InstState& inst = at(insts_, slot);
  inst.greeks = greeks;
  inst.has_greeks = true;
  for (std::int32_t p = inst.head; p >= 0; p = at(positions_, p).next_in_instrument) {
    refresh(p);
  }
  return true;
}

bool ScopeBook::add_working(const Attribution& who, std::int64_t instrument_id, Side side,
                            std::int64_t qty) noexcept {
  const InstrumentRef* ref = instruments_->find(instrument_id);
  if (ref == nullptr) {
    return false;
  }
  if (ref->segment != Segment::kEq && ref->lot_size <= 0) {
    healthy_ = false;
    return false;
  }
  const std::int64_t underlying = ref->underlying_id != 0 ? ref->underlying_id : instrument_id;
  const std::int32_t inst = find_or_add_inst(instrument_id);
  if (inst < 0) {
    healthy_ = false;
    return false;
  }
  const ScopeList list = scopes_of(who);
  for (std::size_t i = 0; i < list.count; ++i) {
    const ScopeRef& scope = list.items[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    const std::int32_t p = find_or_add_position(scope.kind, scope.id, instrument_id, underlying, inst, *ref);
    if (p < 0) {
      healthy_ = false;
      return false;
    }
    Position& pos = at(positions_, p);
    std::int64_t& working = side == Side::kBuy ? pos.working_buy : pos.working_sell;
    if (__builtin_add_overflow(working, qty, &working)) {
      healthy_ = false;
    }
    if (working < 0) {
      healthy_ = false;  // more taken off than was ever put on: the feed is wrong, so refuse everything
    }
    if (side == Side::kSell && pos.segment == Segment::kOpt) {
      ScopeAgg& agg = at(aggs_, pos.agg);
      agg.working_short_lots += lots_at_limit_scale(Wide{qty}, pos.lot_size);
      if (agg.working_short_lots < 0) {
        healthy_ = false;
      }
    }
  }
  return healthy_;
}

bool ScopeBook::add_open_orders(const Attribution& who, std::int64_t delta) noexcept {
  const ScopeList list = scopes_of(who);
  for (std::size_t i = 0; i < list.count; ++i) {
    const ScopeRef& scope = list.items[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    const std::int32_t a = find_or_add_agg(scope.kind, scope.id);
    if (a < 0) {
      healthy_ = false;
      return false;
    }
    ScopeAgg& agg = at(aggs_, a);
    if (__builtin_add_overflow(agg.open_orders, delta, &agg.open_orders)) {
      healthy_ = false;
    }
    if (agg.open_orders < 0) {
      healthy_ = false;
    }
  }
  return healthy_;
}

void ScopeBook::begin_session() noexcept {
  for (std::size_t i = 0; i < agg_count_; ++i) {
    aggs_[i].session_base = aggs_[i].pnl();
    aggs_[i].peak_pnl = 0;
  }
}

bool ScopeBook::held_greeks_fresh(Nanos monotonic, Nanos max_age, Nanos stale_before) const noexcept {
  for (std::size_t i = 0; i < held_count_; ++i) {
    const InstState& state = insts_[static_cast<std::size_t>(held_[i])];
    if (!state.option) {
      continue;  // a share or a future has delta 1 and no vega by definition: it needs no Greeks
    }
    const Nanos age = monotonic - state.greeks.as_of;
    if (!state.has_greeks || state.greeks.as_of <= stale_before || age < 0 || age > max_age) {
      return false;
    }
  }
  return true;
}

namespace {

[[nodiscard]] bool mark_usable(bool has_mark, Nanos as_of, const ScopeBook::MarkWindow& w,
                               Nanos max_age) noexcept {
  const Nanos age = w.monotonic - as_of;
  return has_mark && as_of > w.stale_before && age >= 0 && age <= max_age;
}

}  // namespace

std::optional<ScopeBook::StaleMark> ScopeBook::stale_mark(std::int64_t own, std::int64_t underlying,
                                                          const MarkWindow& window) const noexcept {
  const auto near_stale = [&](std::int64_t id) noexcept {
    const std::int32_t slot = find_inst(id);
    if (slot < 0) {
      return true;
    }
    const InstState& state = insts_[static_cast<std::size_t>(slot)];
    return !mark_usable(state.has_mark, state.mark_as_of, window, window.near_age);
  };
  if (near_stale(own)) {
    return StaleMark{.instrument_id = own, .max_age = window.near_age};
  }
  const bool has_underlying = underlying != 0 && underlying != own;
  if (has_underlying && near_stale(underlying)) {
    return StaleMark{.instrument_id = underlying, .max_age = window.near_age};
  }
  // own and the underlying passed the near age. Unless the other age is the shorter, that is enough for the
  // other one too, so the scan need not tell them apart.
  const bool skip_near = window.other_age < window.near_age;
  for (std::size_t i = 0; i < held_count_; ++i) {
    const InstState& state = insts_[static_cast<std::size_t>(held_[i])];
    if (skip_near && (state.instrument_id == own || (has_underlying && state.instrument_id == underlying))) {
      continue;  // judged above, at the near age
    }
    if (!mark_usable(state.has_mark, state.mark_as_of, window, window.other_age)) {
      return StaleMark{.instrument_id = state.instrument_id, .max_age = window.other_age};
    }
  }
  return std::nullopt;
}

const Position* ScopeBook::position(LimitScope kind, const Uuid& scope,
                                    std::int64_t instrument_id) const noexcept {
  const std::int32_t slot = find_position(kind, scope, instrument_id);
  return slot < 0 ? nullptr : &positions_[static_cast<std::size_t>(slot)];
}

const ScopeAgg* ScopeBook::agg(LimitScope kind, const Uuid& scope) const noexcept {
  const std::int32_t slot = find_agg(kind, scope);
  return slot < 0 ? nullptr : &aggs_[static_cast<std::size_t>(slot)];
}

Wide ScopeBook::underlying_notional(LimitScope kind, const Uuid& scope,
                                    std::int64_t underlying_id) const noexcept {
  const std::int32_t slot = find_underlying(kind, scope, underlying_id);
  return slot < 0 ? Wide{0} : underlyings_[static_cast<std::size_t>(slot)].notional;
}

namespace {

// What a futures position covers, in units: a long future covers short calls and a short future covers short
// puts, unit for unit (D-31), less what working orders on the closing side would take away when they fill.
[[nodiscard]] Wide future_cover(const Position& p, OptionKind kind) noexcept {
  const Wide net = p.net_qty;
  const Wide held = kind == OptionKind::kCall ? net - Wide{p.working_sell} : -net - Wide{p.working_buy};
  return held > 0 ? held : Wide{0};
}

// The longs and shorts of one kind of option by expiry, and the futures that cover that kind by expiry, for
// the cover checks. Units of the underlying throughout. Small fixed tables: more expiries than they hold
// can't be answered.
class ExpiryTallies {
 public:
  static constexpr std::size_t kMax = 16;
  struct Tally {
    Nanos expiry{0};
    Wide longs{0};
    Wide shorts{0};
    [[nodiscard]] Wide uncovered() const noexcept { return shorts > longs ? shorts - longs : Wide{0}; }
  };
  struct Future {
    Nanos expiry{0};
    Wide units{0};
  };

  // False when the expiry is new and the table is full.
  [[nodiscard]] bool add(Nanos expiry, Wide longs, Wide shorts) noexcept {
    total_shorts_ += shorts;
    for (Tally& t : std::span(tallies_).first(used_)) {
      if (t.expiry == expiry) {
        t.longs += longs;
        t.shorts += shorts;
        return true;
      }
    }
    if (used_ == kMax) {
      return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): checked against kMax above
    tallies_[used_++] = Tally{.expiry = expiry, .longs = longs, .shorts = shorts};
    return true;
  }
  [[nodiscard]] bool add_future(Nanos expiry, Wide units) noexcept {
    for (Future& f : std::span(futures_).first(futures_used_)) {
      if (f.expiry == expiry) {
        f.units += units;
        return true;
      }
    }
    if (futures_used_ == kMax) {
      return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): checked against kMax above
    futures_[futures_used_++] = Future{.expiry = expiry, .units = units};
    return true;
  }

  [[nodiscard]] Wide total_shorts() const noexcept { return total_shorts_; }
  // The group for `expiry` goes to the view's own figures; the others' shorts that their longs don't cover
  // are counted aside, as they would take the futures first.
  void settle(Nanos expiry, ScopeBook::CoverView& view) const noexcept {
    for (const Tally& t : std::span(tallies_).first(used_)) {
      if (t.expiry == expiry) {
        view.long_units = t.longs;
        view.short_units = t.shorts;
      } else {
        view.others_uncovered += t.uncovered();
      }
    }
    for (const Future& f : std::span(futures_).first(futures_used_)) {
      if (f.expiry >= expiry) {
        view.future_units += f.units;
      }
    }
  }

  // How far the shorts are from all being covered: 0 when they are. A future covers options expiring at or
  // before it, so the shorts expiring at or after any date T can only use the futures expiring at or after
  // T, and the most that can go uncovered is the largest, over every T, of what those shorts' own longs
  // leave over less those futures. `need` and `have` are that T's two sides.
  struct Shortfall {
    Wide units{0};
    Wide need{0};
    Wide have{0};
  };
  [[nodiscard]] Shortfall worst_shortfall() const noexcept {
    Shortfall worst;
    for (const Tally& from : std::span(tallies_).first(used_)) {
      Wide need = 0;
      for (const Tally& t : std::span(tallies_).first(used_)) {
        if (t.expiry >= from.expiry) {
          need += t.uncovered();
        }
      }
      Wide have = 0;
      for (const Future& f : std::span(futures_).first(futures_used_)) {
        if (f.expiry >= from.expiry) {
          have += f.units;
        }
      }
      if (need - have > worst.units) {
        worst = {.units = need - have, .need = need, .have = have};
      }
    }
    return worst;
  }

 private:
  std::array<Tally, kMax> tallies_{};
  std::size_t used_{0};
  std::array<Future, kMax> futures_{};
  std::size_t futures_used_{0};
  Wide total_shorts_{0};
};

// One position's part of the tallies. False when there are more expiries than the tables hold; the shorts
// are still added up, so the caller can tell whether there is any short to worry about.
[[nodiscard]] bool take_position(const Position& p, OptionKind kind, ExpiryTallies& tallies) noexcept {
  if (p.segment == Segment::kFut) {
    const Wide cover = future_cover(p, kind);
    return cover == 0 || tallies.add_future(p.expires_at, cover);
  }
  if (p.segment != Segment::kOpt || p.option_kind != kind) {
    return true;
  }
  // Working buy orders aren't cover, and a working sell has to be covered too.
  const Wide net = p.net_qty;
  const Wide longs = net > 0 ? net : Wide{0};
  const Wide shorts = (net < 0 ? -net : Wide{0}) + Wide{p.working_sell};
  if (longs == 0 && shorts == 0) {
    return true;  // flat and nothing working: no reason to take a place in the table
  }
  return tallies.add(p.expires_at, longs, shorts);
}

}  // namespace

ScopeBook::CoverView ScopeBook::cover_view(const Uuid& account, std::int64_t underlying_id, OptionKind kind,
                                           Nanos expiry) const noexcept {
  CoverView view;
  const std::int32_t under = find_underlying(LimitScope::kAccount, account, underlying_id);
  if (under < 0) {
    return view;
  }
  ExpiryTallies tallies;
  for (std::int32_t i = underlyings_[static_cast<std::size_t>(under)].head; i >= 0;
       i = positions_[static_cast<std::size_t>(i)].next_in_underlying) {
    if (!take_position(positions_[static_cast<std::size_t>(i)], kind, tallies)) {
      view.overflowed = true;
      return view;
    }
  }
  tallies.settle(expiry, view);
  return view;
}

ScopeBook::Uncover ScopeBook::uncover(const Uuid& account, std::int64_t instrument_id, Side side,
                                      std::int64_t qty) const noexcept {
  const std::int32_t slot = find_position(LimitScope::kAccount, account, instrument_id);
  if (slot < 0 || qty <= 0) {
    return {};
  }
  const Position& own = positions_[static_cast<std::size_t>(slot)];
  const bool sell = side == Side::kSell;
  // What the position covers, and how much of that the order takes away.
  OptionKind kind = OptionKind::kNone;
  Wide taken = 0;
  if (own.segment == Segment::kFut) {
    kind = sell ? OptionKind::kCall : OptionKind::kPut;  // selling a long future, or buying a short one back
    taken = std::min(future_cover(own, kind), Wide{qty});
  } else if (own.segment == Segment::kOpt && sell && own.net_qty > 0) {
    kind = own.option_kind;
    taken = std::min(Wide{qty}, Wide{own.net_qty});  // it can sell no more than it holds: the rest is opening
  }
  if (kind == OptionKind::kNone || taken <= 0) {
    return {};
  }
  ExpiryTallies tallies;
  bool overflowed = false;
  for (std::int32_t i = at(underlyings_, own.underlying).head; i >= 0;
       i = positions_[static_cast<std::size_t>(i)].next_in_underlying) {
    overflowed = !take_position(positions_[static_cast<std::size_t>(i)], kind, tallies) || overflowed;
  }
  if (tallies.total_shorts() == 0) {
    return {};  // no short of that kind, so nothing to leave uncovered
  }
  if (overflowed) {
    return {.uncovers = true, .need = tallies.total_shorts(), .have = 0};
  }
  // Uncovering is leaving more short units without cover than there were: a short that had none already is
  // not made worse by an order that takes nothing from it.
  const Wide before = tallies.worst_shortfall().units;
  if (own.segment == Segment::kFut) {
    static_cast<void>(tallies.add_future(own.expires_at, -taken));  // its expiry is in the table already
  } else {
    static_cast<void>(tallies.add(own.expires_at, 0, taken));  // a long sold is a unit more to find cover for
  }
  const auto after = tallies.worst_shortfall();
  if (after.units > before) {
    return {.uncovers = true, .need = after.need, .have = after.have};
  }
  return {};
}

const Greeks* ScopeBook::greeks(std::int64_t instrument_id) const noexcept {
  const std::int32_t slot = find_inst(instrument_id);
  if (slot < 0 || !insts_[static_cast<std::size_t>(slot)].has_greeks) {
    return nullptr;
  }
  return &insts_[static_cast<std::size_t>(slot)].greeks;
}

}  // namespace vyuha::risk

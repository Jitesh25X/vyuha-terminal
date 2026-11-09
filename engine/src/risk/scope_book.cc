#include "vyuha/risk/scope_book.h"

#include <algorithm>

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
                                             std::int64_t underlying_id, std::int32_t inst) noexcept {
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
  at(positions_, slot) = Position{.kind = kind,
                                  .scope = scope,
                                  .instrument_id = instrument_id,
                                  .agg = agg,
                                  .underlying = under,
                                  .next_in_instrument = state.head};
  state.head = slot;
  position_index_.insert(hash_key(kind, scope, instrument_id), slot);
  return slot;
}

// Recomputes what a position adds to its scope, from its net quantity, the mark and the Greeks, and moves the
// scope's aggregates by the difference.
void ScopeBook::refresh(std::int32_t position) noexcept {
  Position& p = at(positions_, position);
  const InstrumentRef* ref = instruments_->find(p.instrument_id);
  const std::int32_t slot = find_inst(p.instrument_id);
  const InstState* inst = slot >= 0 ? &at(insts_, slot) : nullptr;
  const bool has_mark = inst != nullptr && inst->has_mark;
  const Wide mark = has_mark ? Wide{inst->mark.units()} : Wide{0};
  const bool option = ref != nullptr && ref->segment == Segment::kOpt;
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
  // An option is held while any scope holds it: the desk can be flat while an account and another carry
  // offsetting positions, and each of them still has its delta and vega in its totals.
  if (option && slot >= 0 && (net != 0) != p.held) {
    InstState& state = at(insts_, slot);
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
  const std::int64_t underlying = ref->underlying_id != 0 ? ref->underlying_id : fill.instrument_id;
  const std::int32_t inst = find_or_add_inst(fill.instrument_id);
  if (inst < 0) {
    healthy_ = false;
    return false;
  }
  const ScopeList list = scopes_of(fill.who);
  for (std::size_t i = 0; i < list.count; ++i) {
    const ScopeRef& scope = list.items[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    const std::int32_t p = find_or_add_position(scope.kind, scope.id, fill.instrument_id, underlying, inst);
    if (p < 0) {
      healthy_ = false;
      return false;
    }
    fill_position(p, fill);
  }
  return true;
}

bool ScopeBook::set_mark(std::int64_t instrument_id, Price mark) noexcept {
  const std::int32_t slot = find_or_add_inst_for_data(instrument_id);
  if (slot < 0) {
    return false;  // nobody holds it and the table is half full: not worth a slot, and not an error
  }
  InstState& inst = at(insts_, slot);
  inst.mark = mark;
  inst.has_mark = mark > Price{};
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
  const std::int64_t underlying = ref->underlying_id != 0 ? ref->underlying_id : instrument_id;
  const std::int32_t inst = find_or_add_inst(instrument_id);
  if (inst < 0) {
    healthy_ = false;
    return false;
  }
  const ScopeList list = scopes_of(who);
  for (std::size_t i = 0; i < list.count; ++i) {
    const ScopeRef& scope = list.items[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    const std::int32_t p = find_or_add_position(scope.kind, scope.id, instrument_id, underlying, inst);
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
    const Nanos age = monotonic - state.greeks.as_of;
    if (!state.has_greeks || state.greeks.as_of <= stale_before || age < 0 || age > max_age) {
      return false;
    }
  }
  return true;
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

const Greeks* ScopeBook::greeks(std::int64_t instrument_id) const noexcept {
  const std::int32_t slot = find_inst(instrument_id);
  if (slot < 0 || !insts_[static_cast<std::size_t>(slot)].has_greeks) {
    return nullptr;
  }
  return &insts_[static_cast<std::size_t>(slot)].greeks;
}

}  // namespace vyuha::risk

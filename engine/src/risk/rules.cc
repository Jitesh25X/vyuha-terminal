#include "vyuha/risk/rules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>

namespace vyuha::risk {

namespace {

using oms::OrderDetails;
using oms::OrderType;

// Prices and quantities come from callers, so the products below can't be allowed to overflow.
__extension__ using Wide = __int128;

constexpr Outcome reject(std::int64_t limit = 0, std::int64_t current = 0) noexcept {
  return {.verdict = Verdict::kReject, .limit = limit, .current = current};
}
constexpr Outcome warn(std::int64_t limit, std::int64_t current) noexcept {
  return {.verdict = Verdict::kWarn, .limit = limit, .current = current};
}

constexpr std::int64_t saturate(Wide value) noexcept {
  return value > std::numeric_limits<std::int64_t>::max() ? std::numeric_limits<std::int64_t>::max()
                                                          : static_cast<std::int64_t>(value);
}

// The LTP if there is one and it arrived within kLtpMaxAge. Too far in the future counts as stale as well.
std::optional<Price> fresh_ltp(const GateContext& ctx, std::int64_t instrument_id, Nanos now) noexcept {
  if (ctx.book == nullptr || instrument_id <= 0) {
    return std::nullopt;
  }
  const md::Tick* tick = ctx.book->find(static_cast<md::InstrumentId>(instrument_id));
  if (tick == nullptr || tick->ltp <= Price{}) {
    return std::nullopt;
  }
  const Nanos age = now - tick->recv_ts;
  return age <= kLtpMaxAge && age >= -kLtpMaxAge ? std::optional{tick->ltp} : std::nullopt;
}

// A hard limit that's breached rejects and a soft one warns. `over` says whether a limit value is breached.
template <class Over>
Outcome judge(const ResolvedLimit& limit, std::int64_t current, Over over) noexcept {
  if (!limit.any()) {
    return reject();  // no limit loaded for this: it can't be checked
  }
  if (limit.hard.has_value() && over(*limit.hard)) {
    return reject(*limit.hard, current);
  }
  if (limit.soft.has_value() && over(*limit.soft)) {
    return warn(*limit.soft, current);
  }
  return {.limit = limit.hard.has_value() ? *limit.hard : limit.soft.value_or(0), .current = current};
}

// HHMM at limit scale, as minutes since midnight. nullopt if it isn't a real time.
constexpr std::optional<std::int32_t> hhmm_minutes(std::int64_t scaled) noexcept {
  if (scaled < 0 || scaled % kLimitScale != 0) {
    return std::nullopt;
  }
  const std::int64_t hhmm = scaled / kLimitScale;
  const std::int64_t hours = hhmm / 100;
  const std::int64_t minutes = hhmm % 100;
  if (hours > 23 || minutes > 59) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>((hours * 60) + minutes);
}

constexpr std::int32_t ist_minute(Nanos now) noexcept {
  return static_cast<std::int32_t>((now - md::ist_day_start(now)) / kNanosPerMinute);
}

// R-MKT-HOURS's check of one HHMM limit. `outside` says whether the minute breaks a limit at that minute.
template <class Outside>
Outcome judge_minute(const ResolvedLimit& limit, std::int32_t minute, Outside outside) noexcept {
  if (!limit.any()) {
    return reject();
  }
  const auto breaks = [&](std::int64_t scaled) noexcept {
    const std::optional<std::int32_t> at = hhmm_minutes(scaled);
    return !at.has_value() || outside(*at);
  };
  if (limit.hard.has_value() && breaks(*limit.hard)) {
    return reject(*limit.hard / kLimitScale, minute);
  }
  if (limit.soft.has_value() && breaks(*limit.soft)) {
    return warn(*limit.soft / kLimitScale, minute);
  }
  return {};
}

// ---- rules 1 to 9: who, when and what ----

Outcome kill(const GateContext& ctx, const OrderDetails& o, const GateFacts& /*f*/) noexcept {
  return ctx.kill.blocks(o) && !o.source.kill_generated() ? reject() : Outcome{};
}

Outcome live_disabled(const GateContext& ctx, const OrderDetails& o, const GateFacts& /*f*/) noexcept {
  if (o.is_paper.paper()) {
    return {};
  }
  if (!ctx.live_trading_enabled) {
    return reject();
  }
  if (o.strategy_id.has_value()) {
    return ctx.live_strategies.contains(*o.strategy_id) ? Outcome{} : reject();
  }
  return o.source == oms::Source::kStrategy ? reject()
                                            : Outcome{};  // a strategy order that names no strategy
}

Outcome entitle(const GateContext& ctx, const OrderDetails& o, const GateFacts& /*f*/) noexcept {
  const Entitlement* e = ctx.entitlements.find(o.user_id, o.account_id);
  if (e == nullptr || !e->can_trade || (o.is_paper.live() && !e->can_live)) {
    return reject();
  }
  return {};
}

Outcome session(const GateContext& /*ctx*/, const OrderDetails& o, const GateFacts& f) noexcept {
  if (o.is_paper.paper()) {
    return {};
  }
  return f.account != nullptr && f.account->session_connected && f.now < f.account->token_expires_at
             ? Outcome{}
             : reject();
}

Outcome mkt_hours(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (!f.opening || f.instrument == nullptr) {
    return {};  // an unknown instrument is R-INSTR-EXPIRED's to refuse
  }
  const SessionToday day = ctx.calendar.session(f.instrument->exchange, f.now);
  const std::int32_t minute = ist_minute(f.now);
  if (!day.known || !day.open) {
    return reject(0, minute);
  }
  if (minute < day.open_minute) {
    return reject(day.open_minute, minute);
  }
  if (minute >= day.close_minute) {
    return reject(day.close_minute, minute);
  }
  // A soft limit only warns, so it must not hide a hard check after it. The first warning is the one
  // reported.
  Outcome warned{};
  const auto note = [&warned](const Outcome& c) noexcept {
    if (c.verdict == Verdict::kWarn && warned.verdict == Verdict::kPass) {
      warned = c;
    }
  };
  // The desk window is about the regular session: a special session is open only for its own hours.
  if (!day.special) {
    const Outcome start =
        judge_minute(ctx.limits.resolve(LimitKey::kTradingWindowStart, o, Tightest::kLargest), minute,
                     [minute](std::int32_t at) noexcept { return minute < at; });
    if (start.verdict == Verdict::kReject) {
      return start;
    }
    note(start);
    const Outcome end = judge_minute(ctx.limits.resolve(LimitKey::kTradingWindowEnd, o, Tightest::kSmallest),
                                     minute, [minute](std::int32_t at) noexcept { return minute >= at; });
    if (end.verdict == Verdict::kReject) {
      return end;
    }
    note(end);
  }
  if (o.product == oms::Product::kMis) {
    const Outcome mis = judge_minute(ctx.limits.resolve(LimitKey::kAllowMisAfter, o, Tightest::kSmallest),
                                     minute, [minute](std::int32_t at) noexcept { return minute >= at; });
    if (mis.verdict == Verdict::kReject) {
      return mis;
    }
    note(mis);
  }
  return warned;
}

Outcome instr_ban(const GateContext& /*ctx*/, const OrderDetails& /*o*/, const GateFacts& f) noexcept {
  return f.opening && f.instrument != nullptr && f.instrument->fo_ban ? reject() : Outcome{};
}

Outcome instr_restricted(const GateContext& /*ctx*/, const OrderDetails& /*o*/, const GateFacts& f) noexcept {
  return f.instrument != nullptr && f.instrument->restricted ? reject() : Outcome{};
}

Outcome instr_expired(const GateContext& /*ctx*/, const OrderDetails& /*o*/, const GateFacts& f) noexcept {
  const InstrumentRef* i = f.instrument;
  if (i == nullptr || i->lot_size <= 0 || i->tick_size <= Price{}) {
    return reject();  // an unknown contract, or a master row that can't be checked against
  }
  return i->expires_at != 0 && f.now >= i->expires_at ? reject() : Outcome{};
}

constexpr bool on_grid(Price p, Price tick) noexcept { return p > Price{} && p.units() % tick.units() == 0; }

Outcome order_shape(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  const InstrumentRef* i = f.instrument;
  if (i == nullptr || i->lot_size <= 0 || i->tick_size <= Price{}) {
    return reject();
  }
  if (o.qty <= 0 || o.qty % i->lot_size != 0) {
    return reject(i->lot_size, o.qty);
  }
  const bool wants_price = o.order_type == OrderType::kLimit || o.order_type == OrderType::kSl;
  const bool wants_trigger = o.order_type == OrderType::kSl || o.order_type == OrderType::kSlM;
  if (o.price.has_value() != wants_price || o.trigger_price.has_value() != wants_trigger) {
    return reject();
  }
  if (o.price.has_value() && !on_grid(*o.price, i->tick_size)) {
    return reject(i->tick_size.units(), o.price->units());
  }
  if (o.trigger_price.has_value() && !on_grid(*o.trigger_price, i->tick_size)) {
    return reject(i->tick_size.units(), o.trigger_price->units());
  }
  if (o.order_type == OrderType::kMarket) {
    return judge(ctx.limits.resolve(LimitKey::kAllowMarketOrders, o, Tightest::kSmallest), 1,
                 [](std::int64_t allowed) noexcept { return allowed <= 0; });
  }
  return {};
}

// ---- rules 10 and 11: price and size ----

Outcome fat_finger(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  const InstrumentRef* known = f.instrument;
  const bool has_band =
      known != nullptr && known->lower_circuit.has_value() && known->upper_circuit.has_value();
  if (o.is_paper.live() && !has_band) {
    return reject();  // no live order of any type without a known band (D-22)
  }
  if (!o.price.has_value()) {
    return {};
  }
  if (known == nullptr) {
    return reject();
  }
  const std::optional<Price> ltp = fresh_ltp(ctx, o.instrument_id, f.now);
  if (!ltp.has_value()) {
    return reject();  // no LTP or a stale one (D-22)
  }
  const InstrumentRef& i = *known;
  if (has_band) {
    if (*o.price < *i.lower_circuit) {
      return reject(i.lower_circuit->units(), o.price->units());
    }
    if (*o.price > *i.upper_circuit) {
      return reject(i.upper_circuit->units(), o.price->units());
    }
  }
  const LimitKey key = i.segment == Segment::kOpt ? LimitKey::kFatFingerPctOpt : LimitKey::kFatFingerPct;
  // Percent at limit scale: |price - ltp| * 100 * scale compared with ltp * pct.
  const Wide deviation = Wide{std::llabs(o.price->units() - ltp->units())} * 100 * kLimitScale;
  const Wide base = ltp->units();
  return judge(ctx.limits.resolve(key, o, Tightest::kSmallest), saturate(deviation / base),
               [deviation, base](std::int64_t pct) noexcept { return deviation > base * pct; });
}

Outcome order_value(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (f.instrument == nullptr || f.instrument->lot_size <= 0) {
    return reject();
  }
  // A limit or stop-limit is valued at its price, a MARKET order at the LTP. A stop-market can fill at the
  // market, so it takes the higher of its trigger and the LTP: a low trigger can't understate it.
  std::optional<Price> at = o.price;
  if (o.order_type == OrderType::kMarket || o.order_type == OrderType::kSlM) {
    at = fresh_ltp(ctx, o.instrument_id, f.now);
    if (o.order_type == OrderType::kSlM && at.has_value() && o.trigger_price.has_value()) {
      at = std::max(*at, *o.trigger_price);
    }
  }
  if (!at.has_value()) {
    return reject();
  }
  const Wide notional = Wide{o.qty} * at->units();
  const Outcome by_value =
      judge(ctx.limits.resolve(LimitKey::kMaxOrderValue, o, Tightest::kSmallest), saturate(notional),
            [notional](std::int64_t v) noexcept { return notional > v; });
  if (by_value.verdict == Verdict::kReject) {
    return by_value;
  }
  const Wide scaled_qty = Wide{o.qty} * kLimitScale;
  const Wide lot = f.instrument->lot_size;
  const Outcome by_lots = judge(ctx.limits.resolve(LimitKey::kMaxOrderQtyLots, o, Tightest::kSmallest),
                                saturate(scaled_qty / lot),
                                [scaled_qty, lot](std::int64_t v) noexcept { return scaled_qty > lot * v; });
  return by_lots.verdict != Verdict::kPass ? by_lots : by_value;
}

// ---- rules 12 and 13: stateful, so two-phase ----

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): bounded by count and the venue is 0 or 1

constexpr Nanos kMaxDupWindow = Nanos{86'400} * kNanosPerSecond;

constexpr Nanos window_ns(std::optional<std::int64_t> scaled) noexcept {
  if (!scaled.has_value() || *scaled <= 0) {
    return 0;
  }
  const std::int64_t ms = *scaled / kLimitScale;
  return ms >= kMaxDupWindow / 1'000'000 ? kMaxDupWindow : ms * 1'000'000;
}

constexpr bool same_order(const DupEntry& e, const OrderDetails& o) noexcept {
  return e.instrument_id == o.instrument_id && e.qty == o.qty && e.side == o.side && e.price == o.price &&
         e.trigger == o.trigger_price && e.account == o.account_id;
}

Outcome dup_check(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  const ResolvedLimit window = ctx.limits.resolve(LimitKey::kDupWindowMs, o, Tightest::kLargest);
  if (!window.any()) {
    return reject();
  }
  const Nanos hard = window_ns(window.hard);
  const Nanos soft = window_ns(window.soft);
  bool warned = false;
  std::size_t live = 0;
  for (const DupEntry& e : std::span(ctx.dup.entries).first(ctx.dup.count)) {
    const Nanos age = f.now - e.ts;
    if (age >= e.keep) {
      continue;  // gone as far as this order is concerned
    }
    ++live;
    if (!same_order(e, o) || (o.slice.sliced() && e.slice == o.slice)) {
      continue;  // not the same order, or slices of one intent (D-24)
    }
    if (age < hard) {  // a clock that stepped back makes age negative: still a duplicate
      return reject(hard / 1'000'000, age / 1'000'000);
    }
    warned = warned || age < soft;
  }
  if (live == DupTable::kCapacity) {
    Outcome full = reject(static_cast<std::int64_t>(DupTable::kCapacity), static_cast<std::int64_t>(live));
    full.alert = true;  // duplicates can't be checked while this lasts (D-24)
    return full;
  }
  return warned ? warn(soft / 1'000'000, 0) : Outcome{};
}

void dup_commit(GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  const ResolvedLimit window = ctx.limits.resolve(LimitKey::kDupWindowMs, o, Tightest::kLargest);
  const Nanos keep = std::max(window_ns(window.hard), window_ns(window.soft));
  DupTable& t = ctx.dup;
  std::size_t kept = 0;
  for (std::size_t i = 0; i < t.count; ++i) {
    if (f.now - t.entries[i].ts < t.entries[i].keep) {
      t.entries[kept++] = t.entries[i];
    }
  }
  t.count = kept;
  if (keep > 0 && t.count < DupTable::kCapacity) {  // check() left room: nothing else writes between the two
    t.entries[t.count++] = DupEntry{.account = o.account_id,
                                    .instrument_id = o.instrument_id,
                                    .side = o.side,
                                    .qty = o.qty,
                                    .price = o.price,
                                    .trigger = o.trigger_price,
                                    .slice = o.slice,
                                    .ts = f.now,
                                    .keep = keep};
  }
}

// A warning cap, as a count of orders per second, within the range a window can check.
constexpr std::uint8_t soft_cap(std::int64_t scaled) noexcept {
  const std::int64_t cap = scaled / kLimitScale;
  return cap <= 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(std::min<std::int64_t>(cap, oms::kMaxOpsCap));
}

Outcome ops_check(const GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  if (f.account == nullptr || f.instrument == nullptr) {
    return reject();
  }
  const std::uint8_t cap = effective_ops_cap(ctx, o.account_id);
  const oms::OpsWindow& window = f.account->ops[ops_venue(f.instrument->exchange)];
  if (!window.has_token(f.now, cap)) {
    return reject(cap, cap);  // refused at once: the core thread can't wait (D-21)
  }
  const ResolvedLimit limit =
      ctx.limits.resolve_account(LimitKey::kOpsCap, o.account_id, Tightest::kSmallest);
  if (limit.soft.has_value() && !window.has_token(f.now, soft_cap(*limit.soft))) {
    return warn(*limit.soft / kLimitScale, 0);
  }
  return {};
}

void ops_commit(GateContext& ctx, const OrderDetails& o, const GateFacts& f) noexcept {
  AccountState* account = ctx.accounts.find(o.account_id);
  if (account != nullptr && f.instrument != nullptr) {  // check() required both
    account->ops[ops_venue(f.instrument->exchange)].take(f.now);
  }
}

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

constexpr std::array<Rule, 13> kPreTradeRules{{
    {.code = RuleCode::kKill, .eval = &kill},
    {.code = RuleCode::kLiveDisabled, .eval = &live_disabled},
    {.code = RuleCode::kEntitle, .eval = &entitle},
    {.code = RuleCode::kSession, .eval = &session},
    {.code = RuleCode::kMktHours, .eval = &mkt_hours},
    {.code = RuleCode::kInstrBan, .eval = &instr_ban},
    {.code = RuleCode::kInstrRestricted, .eval = &instr_restricted},
    {.code = RuleCode::kInstrExpired, .eval = &instr_expired},
    {.code = RuleCode::kOrderShape, .eval = &order_shape},
    {.code = RuleCode::kFatFinger, .eval = &fat_finger},
    {.code = RuleCode::kOrderValue, .eval = &order_value},
    {.code = RuleCode::kDup, .eval = &dup_check, .on_approved = &dup_commit},
    {.code = RuleCode::kOps, .eval = &ops_check, .on_approved = &ops_commit},
}};

}  // namespace

std::span<const Rule> pre_trade_rules() noexcept { return kPreTradeRules; }

bool live_trading_allowed(std::string_view enabled, std::string_view env) noexcept {
  return enabled == "true" && (env == "prod" || env == "staging");
}

std::uint8_t effective_ops_cap(const GateContext& ctx, const oms::Uuid& account) noexcept {
  const AccountState* row = ctx.accounts.find(account);
  if (row == nullptr) {
    return 0;
  }
  std::int64_t cap = std::min<std::int64_t>(row->ops_cap, oms::kMaxOpsCap);
  const ResolvedLimit limit = ctx.limits.resolve_account(LimitKey::kOpsCap, account, Tightest::kSmallest);
  if (limit.hard.has_value()) {
    cap = std::min(cap, *limit.hard / kLimitScale);
  }
  return cap <= 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(cap);
}

std::uint8_t GateSendLimits::ops_venue(std::int64_t instrument_id) const noexcept {
  const InstrumentRef* i = ctx_->instruments != nullptr ? ctx_->instruments->find(instrument_id) : nullptr;
  return i != nullptr ? risk::ops_venue(i->exchange) : oms::kOpsVenueUnknown;
}

}  // namespace vyuha::risk

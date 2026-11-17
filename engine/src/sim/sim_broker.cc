#include "vyuha/sim/sim_broker.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "vyuha/risk/rules.h"

namespace vyuha::sim {

namespace {

using oms::OrderEvent;
using oms::OrderType;
using oms::Side;

constexpr std::size_t kMaxOrders = 4'194'304;  // indexes are int32 and the pools are summed
constexpr std::size_t kMaxEvents = 16'777'216;
constexpr std::int32_t kMaxSlippageTicks = 10'000;
constexpr std::size_t kMinEvents = 4;  // a send can need two (ack, then a fill or a cancel)

[[nodiscard]] std::uint64_t fnv1a(std::string_view s) noexcept {
  std::uint64_t h = 0xCBF29CE484222325ULL;
  for (const char c : s) {
    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001B3ULL;
  }
  return h;
}

[[nodiscard]] std::size_t buckets_for(std::size_t entries) noexcept {
  return std::bit_ceil(std::max<std::size_t>(entries * 2, 16));
}

[[nodiscard]] bool is_stop(OrderType t) noexcept { return t == OrderType::kSl || t == OrderType::kSlM; }
[[nodiscard]] bool needs_tick_size(OrderType t) noexcept {
  return t == OrderType::kMarket || t == OrderType::kSlM;
}

// "SIM-12" and "SIMT-7", with no allocation.
template <std::size_t N>
[[nodiscard]] bool numbered(oms::FixedString<N>& out, std::string_view prefix, std::uint64_t n) noexcept {
  std::array<char, 48> buf{};
  std::ranges::copy(prefix, buf.begin());
  const auto res = std::to_chars(buf.data() + prefix.size(), buf.data() + buf.size(), n);
  return out.assign(std::string_view(buf.data(), static_cast<std::size_t>(res.ptr - buf.data())));
}

[[nodiscard]] bool malformed(const oms::OrderDetails& o) noexcept {
  if (o.qty <= 0) {
    return true;
  }
  const bool needs_price = o.order_type == OrderType::kLimit || o.order_type == OrderType::kSl;
  if (needs_price && o.price.value_or(Price{}).units() <= 0) {
    return true;
  }
  return is_stop(o.order_type) && o.trigger_price.value_or(Price{}).units() <= 0;
}

[[nodiscard]] bool stop_hit(bool buy, Price ltp, Price trigger) noexcept {
  return buy ? ltp >= trigger : ltp <= trigger;
}

// MARKET: the opposite best (the LTP when that side is empty) plus or minus the slippage. Arithmetic that
// would overflow, or a price that ends at or below zero, is no fill.
[[nodiscard]] std::optional<Price> market_fill(bool buy, Price tick_size, std::int32_t slippage_ticks,
                                               const md::Tick& t) noexcept {
  const Price opposite = buy ? t.ask : t.bid;
  const std::int64_t base = (opposite.units() > 0 ? opposite : t.ltp).units();
  std::int64_t slip = 0;
  if (__builtin_mul_overflow(static_cast<std::int64_t>(slippage_ticks), tick_size.units(), &slip)) {
    return std::nullopt;
  }
  std::int64_t out = 0;
  const bool overflow =
      buy ? __builtin_add_overflow(base, slip, &out) : __builtin_sub_overflow(base, slip, &out);
  if (overflow || out <= 0) {
    return std::nullopt;
  }
  return Price::from_units(out);
}

// LIMIT: marketable fills at the opposite best, traded through at the limit price, a touch by a draw.
[[nodiscard]] std::optional<Price> limit_fill(bool buy, Price limit, const md::Tick& t, SimRng& rng,
                                              std::uint32_t touch_permille) noexcept {
  const bool marketable = buy ? (t.ask.units() > 0 && limit >= t.ask) : (t.bid.units() > 0 && limit <= t.bid);
  if (marketable) {
    return buy ? t.ask : t.bid;  // price improvement, no slippage
  }
  if (buy ? t.ltp < limit : t.ltp > limit) {
    return limit;
  }
  if (t.ltp == limit && rng.hit(touch_permille)) {
    return limit;
  }
  return std::nullopt;
}

}  // namespace

// ---- creation ----

std::expected<SimBroker, Error> SimBroker::create(const risk::InstrumentTable& instruments,
                                                  const md::LastValueBook& book, const IClock& clock,
                                                  const SimConfig& config) {
  const auto bad = [](std::string_view why) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, why});
  };
  if (config.max_orders < 1 || config.max_orders > kMaxOrders) {
    return bad("SimConfig.max_orders must be 1 to 4194304");
  }
  if (config.history < 1 || config.history > kMaxOrders) {
    return bad("SimConfig.history must be 1 to 4194304");
  }
  if (config.slippage_ticks < 0 || config.slippage_ticks > kMaxSlippageTicks) {
    return bad("SimConfig.slippage_ticks must be 0 to 10000");
  }
  if (config.touch_fill_permille > 1000) {
    return bad("SimConfig.touch_fill_permille must be 0 to 1000");
  }
  if (config.event_capacity < kMinEvents || config.event_capacity > kMaxEvents) {
    return bad("SimConfig.event_capacity must be 4 to 16777216");
  }
  return std::expected<SimBroker, Error>(std::in_place, PassKey{}, instruments, book, clock, config);
}

SimBroker::SimBroker(PassKey /*key*/, const risk::InstrumentTable& instruments, const md::LastValueBook& book,
                     const IClock& clock, const SimConfig& config)
    : instruments_(&instruments),
      book_(&book),
      clock_(&clock),
      config_(config),
      rng_(config.seed),
      recs_(config.max_orders + config.history),
      tags_(buckets_for(config.max_orders + config.history)),
      cids_(buckets_for(config.max_orders + config.history)),
      insts_(buckets_for(config.max_orders)),
      events_(config.event_capacity) {
  for (std::size_t i = config.max_orders; i-- > 0;) {  // free list, lowest index first out
    recs_[i].inst_next = free_head_;
    free_head_ = static_cast<std::int32_t>(i);
  }
}

// ---- chains ----

template <auto Next>
void SimBroker::link(Chain& chain, std::size_t bucket, std::int32_t idx) noexcept {
  recs_[static_cast<std::size_t>(idx)].*Next = kNone;
  if (chain.tail[bucket] == kNone) {
    chain.head[bucket] = idx;
  } else {
    recs_[static_cast<std::size_t>(chain.tail[bucket])].*Next = idx;
  }
  chain.tail[bucket] = idx;
}

template <auto Next>
void SimBroker::unlink(Chain& chain, std::size_t bucket, std::int32_t idx) noexcept {
  std::int32_t prev = kNone;
  for (std::int32_t i = chain.head[bucket]; i != kNone;
       prev = i, i = recs_[static_cast<std::size_t>(i)].*Next) {
    if (i != idx) {
      continue;
    }
    const std::int32_t after = recs_[static_cast<std::size_t>(i)].*Next;
    if (prev == kNone) {
      chain.head[bucket] = after;
    } else {
      recs_[static_cast<std::size_t>(prev)].*Next = after;
    }
    if (chain.tail[bucket] == idx) {
      chain.tail[bucket] = prev;
    }
    recs_[static_cast<std::size_t>(idx)].*Next = kNone;
    return;
  }
}

std::size_t SimBroker::tag_bucket(std::string_view tag) const noexcept {
  return static_cast<std::size_t>(fnv1a(tag)) & tags_.mask;
}
std::size_t SimBroker::cid_bucket(std::string_view cid) const noexcept {
  return static_cast<std::size_t>(fnv1a(cid)) & cids_.mask;
}
std::size_t SimBroker::inst_bucket(std::int64_t instrument_id) const noexcept {
  // Fibonacci hashing spreads sequential ids, as md::IdMap does.
  return static_cast<std::size_t>((static_cast<std::uint64_t>(instrument_id) * 0x9E3779B97F4A7C15ULL) >>
                                  32U) &
         insts_.mask;
}

void SimBroker::link_inst(std::size_t bucket, std::int32_t idx) noexcept {
  Rec& r = recs_[static_cast<std::size_t>(idx)];
  r.inst_next = kNone;
  r.inst_prev = insts_.tail[bucket];
  if (insts_.tail[bucket] == kNone) {
    insts_.head[bucket] = idx;
  } else {
    recs_[static_cast<std::size_t>(insts_.tail[bucket])].inst_next = idx;
  }
  insts_.tail[bucket] = idx;
}

// O(1): the instrument chain is doubly linked, so k fills among n orders cost k, not k * n.
void SimBroker::unlink_inst(std::size_t bucket, std::int32_t idx) noexcept {
  Rec& r = recs_[static_cast<std::size_t>(idx)];
  if (r.inst_prev == kNone) {
    insts_.head[bucket] = r.inst_next;
  } else {
    recs_[static_cast<std::size_t>(r.inst_prev)].inst_next = r.inst_next;
  }
  if (r.inst_next == kNone) {
    insts_.tail[bucket] = r.inst_prev;
  } else {
    recs_[static_cast<std::size_t>(r.inst_next)].inst_prev = r.inst_prev;
  }
  r.inst_next = kNone;
  r.inst_prev = kNone;
}

std::int32_t SimBroker::find_tag(std::string_view tag) const noexcept {
  for (std::int32_t i = tags_.head[tag_bucket(tag)]; i != kNone;
       i = recs_[static_cast<std::size_t>(i)].tag_next) {
    if (recs_[static_cast<std::size_t>(i)].tag.view() == tag) {
      return i;
    }
  }
  return kNone;
}

std::int32_t SimBroker::find_cid(std::string_view cid) const noexcept {
  for (std::int32_t i = cids_.head[cid_bucket(cid)]; i != kNone;
       i = recs_[static_cast<std::size_t>(i)].cid_next) {
    if (recs_[static_cast<std::size_t>(i)].client_order_id.view() == cid) {
      return i;
    }
  }
  return kNone;
}

// ---- events ----

void SimBroker::emit(OrderEvent kind, const Rec& rec, std::int32_t fill_qty, Price fill_price,
                     std::string_view trade_id, std::string_view reason) noexcept {
  SimEvent& e = events_[(ev_head_ + ev_count_) % events_.size()];
  ++ev_count_;
  e.kind = kind;
  e.client_order_id = rec.client_order_id;
  e.order_id = rec.order_id;
  e.broker_order_id = rec.broker_order_id;
  e.fill_qty = fill_qty;
  e.fill_price = fill_price;
  const bool ok = e.broker_trade_id.assign(trade_id);  // at most 40 bytes: our own "SIMT-<n>"
  (void)ok;
  e.reason_code = reason;
}

bool SimBroker::pop_event(SimEvent& out) noexcept {
  const SimEvent* e = peek_event();
  if (e == nullptr) {
    return false;
  }
  out = *e;
  discard_event();
  return true;
}

const SimEvent* SimBroker::peek_event() const noexcept {
  return ev_count_ == 0 ? nullptr : &events_[ev_head_];
}

void SimBroker::discard_event() noexcept {
  if (ev_count_ == 0) {
    return;
  }
  ev_head_ = (ev_head_ + 1) % events_.size();
  --ev_count_;
}

// ---- orders ----

std::int32_t SimBroker::claim_history() noexcept {
  const auto idx = static_cast<std::int32_t>(config_.max_orders + hist_pos_);
  Rec& slot = recs_[static_cast<std::size_t>(idx)];
  if (slot.state != State::kFree) {  // the oldest ended order falls out of the ring
    unlink<&Rec::tag_next>(tags_, tag_bucket(slot.tag.view()), idx);
    unlink<&Rec::cid_next>(cids_, cid_bucket(slot.client_order_id.view()), idx);
    slot.state = State::kFree;
  }
  hist_pos_ = (hist_pos_ + 1) % config_.history;
  return idx;
}

// The order has ended (the caller emitted its event): off the working lists, into the history ring.
void SimBroker::retire(std::int32_t working_idx, State to) noexcept {
  Rec& w = recs_[static_cast<std::size_t>(working_idx)];
  unlink<&Rec::tag_next>(tags_, tag_bucket(w.tag.view()), working_idx);
  unlink<&Rec::cid_next>(cids_, cid_bucket(w.client_order_id.view()), working_idx);
  unlink_inst(inst_bucket(w.instrument_id), working_idx);
  --working_;
  const std::int32_t h = claim_history();
  Rec& dst = recs_[static_cast<std::size_t>(h)];
  dst = w;
  dst.state = to;
  link<&Rec::tag_next>(tags_, tag_bucket(dst.tag.view()), h);
  link<&Rec::cid_next>(cids_, cid_bucket(dst.client_order_id.view()), h);
  w.state = State::kFree;
  w.inst_next = free_head_;
  free_head_ = working_idx;
}

void SimBroker::complete(std::int32_t idx, Price price) noexcept {
  Rec& r = recs_[static_cast<std::size_t>(idx)];
  r.filled_qty = r.qty;
  r.average = price;
  oms::FixedString<40> trade;
  const bool ok = numbered(trade, "SIMT-", ++trade_seq_);
  (void)ok;
  emit(OrderEvent::kFill, r, r.qty, price, trade.view(), {});
  ++stats_.fills;
  retire(idx, State::kFilled);
}

const md::Tick* SimBroker::usable_tick(std::int64_t instrument_id) const noexcept {
  const md::Tick* t = book_->find(static_cast<md::InstrumentId>(instrument_id));
  if (t == nullptr || t->ltp.units() <= 0) {
    return nullptr;
  }
  const Nanos age = clock_->monotonic() - t->recv_mono;
  return age >= 0 && age <= risk::kLtpMaxAge ? t : nullptr;
}

std::optional<Price> SimBroker::match(Rec& r, const md::Tick& t) noexcept {
  if (t.ltp.units() <= 0) {
    return std::nullopt;
  }
  const bool buy = r.side == Side::kBuy;
  OrderType type = r.type;
  if (is_stop(type)) {
    if (!r.triggered) {
      if (!stop_hit(buy, t.ltp, r.trigger.value_or(Price{}))) {
        return std::nullopt;
      }
      r.triggered = true;
    }
    type = type == OrderType::kSlM ? OrderType::kMarket : OrderType::kLimit;
  }
  if (type == OrderType::kMarket) {
    return market_fill(buy, r.tick_size, config_.slippage_ticks, t);
  }
  return limit_fill(buy, r.price.value_or(Price{}), t, rng_, config_.touch_fill_permille);
}

bool SimBroker::send(const risk::ApprovedOrder& approved, std::string_view tag) noexcept {
  if (approved.spent()) {
    ++stats_.refused_spent;
    return false;
  }
  const oms::OrderDetails& o = approved.order();
  if (!o.is_paper.paper()) {  // invariant 3: the SimBroker never takes a live order
    ++stats_.refused_live;
    return false;
  }
  if (tag.empty() || tag.size() > oms::FixedString<40>::capacity() || o.client_order_id.empty()) {
    ++stats_.refused_tag;
    return false;
  }
  if (const std::int32_t held = find_tag(tag); held != kNone) {
    if (recs_[static_cast<std::size_t>(held)].client_order_id == o.client_order_id) {
      return true;  // already handed over
    }
    ++stats_.refused_tag;  // a different order under this tag: not held, so not "handed over"
    return false;
  }
  const risk::InstrumentRef* instrument = instruments_->find(o.instrument_id);
  if (instrument == nullptr || (needs_tick_size(o.order_type) && instrument->tick_size.units() <= 0)) {
    ++stats_.refused_instrument;
    return false;
  }
  const bool invalid = malformed(o);
  if (!invalid && free_head_ == kNone) {
    ++stats_.refused_full;
    return false;
  }
  if (events_free() < 2) {
    ++stats_.queue_full;
    return false;
  }

  const std::uint64_t n = ++order_seq_;
  std::int32_t idx = kNone;
  if (invalid) {
    idx = claim_history();
  } else {
    idx = free_head_;
    free_head_ = recs_[static_cast<std::size_t>(idx)].inst_next;
  }
  Rec& r = recs_[static_cast<std::size_t>(idx)];
  r = Rec{};
  const bool ok_tag = r.tag.assign(tag);
  const bool ok_cid = r.client_order_id.assign(o.client_order_id.view());
  (void)ok_tag;  // sizes were checked above
  (void)ok_cid;
  r.order_id = o.id;
  r.instrument_id = o.instrument_id;
  r.tick_size = instrument->tick_size;
  r.side = o.side;
  r.type = o.order_type;
  r.validity = o.validity;
  r.qty = o.qty;
  r.price = o.price;
  r.trigger = o.trigger_price;

  if (invalid) {
    r.state = State::kRejected;
    link<&Rec::tag_next>(tags_, tag_bucket(tag), idx);
    link<&Rec::cid_next>(cids_, cid_bucket(r.client_order_id.view()), idx);
    emit(OrderEvent::kBrokerReject, r, 0, {}, {}, kReasonInvalid);
    ++stats_.rejected_invalid;
    return true;
  }

  const bool ok_id = numbered(r.broker_order_id, "SIM-", n);
  (void)ok_id;
  r.state = State::kWorking;
  link<&Rec::tag_next>(tags_, tag_bucket(tag), idx);
  link<&Rec::cid_next>(cids_, cid_bucket(r.client_order_id.view()), idx);
  link_inst(inst_bucket(r.instrument_id), idx);
  ++working_;
  emit(OrderEvent::kBrokerAck, r, 0, {}, {}, {});

  if (const md::Tick* last = usable_tick(r.instrument_id); last != nullptr) {
    if (const std::optional<Price> price = match(r, *last); price.has_value()) {
      complete(idx, *price);
      return true;
    }
  }
  if (r.validity == oms::Validity::kIoc) {
    emit(OrderEvent::kBrokerCancel, r, 0, {}, {}, kReasonIocRemainder);
    retire(idx, State::kCancelled);
  }
  return true;
}

bool SimBroker::on_tick(const md::Tick& tick) noexcept {
  if (tick.ltp.units() <= 0) {
    return true;
  }
  const auto instrument = static_cast<std::int64_t>(tick.instrument_id);
  std::int32_t next = kNone;
  for (std::int32_t i = insts_.head[inst_bucket(instrument)]; i != kNone; i = next) {
    Rec& r = recs_[static_cast<std::size_t>(i)];
    next = r.inst_next;  // a fill takes the order off the chain
    if (r.instrument_id != instrument) {
      continue;
    }
    if (events_free() < 1) {
      ++stats_.queue_full;
      return false;
    }
    if (const std::optional<Price> price = match(r, tick); price.has_value()) {
      complete(i, *price);
    }
  }
  return true;
}

bool SimBroker::cancel(const oms::Order& order) noexcept {
  const std::int32_t idx = find_cid(order.details.client_order_id.view());
  if (idx == kNone) {
    ++stats_.cancel_unknown;
    return false;
  }
  const Rec& r = recs_[static_cast<std::size_t>(idx)];
  if (!order.broker_order_id.empty() && !r.broker_order_id.empty() &&
      order.broker_order_id != r.broker_order_id) {
    ++stats_.cancel_unknown;  // the OMS means another order
    return false;
  }
  if (events_free() < 1) {
    ++stats_.queue_full;
    return false;
  }
  if (r.state == State::kWorking) {
    emit(OrderEvent::kCancelConfirm, r, 0, {}, {}, {});
    retire(idx, State::kCancelled);
  } else if (r.state == State::kCancelled) {
    emit(OrderEvent::kCancelConfirm, r, 0, {}, {}, {});  // idempotent: it is cancelled, as asked
  } else {
    emit(OrderEvent::kCancelReject, r, 0, {}, {}, kReasonTooLate);  // it filled (or was rejected)
  }
  return true;
}

std::expected<std::vector<oms::BrokerOrderView>, Error> SimBroker::find_by_tag(std::string_view tag) {
  std::vector<oms::BrokerOrderView> out;
  for (std::int32_t i = tags_.head[tag_bucket(tag)]; i != kNone;
       i = recs_[static_cast<std::size_t>(i)].tag_next) {
    const Rec& r = recs_[static_cast<std::size_t>(i)];
    if (r.tag.view() != tag) {
      continue;
    }
    using BrokerState = oms::BrokerOrderView::State;
    BrokerState state = BrokerState::kOpen;
    switch (r.state) {
      case State::kFilled:
        state = BrokerState::kFilled;
        break;
      case State::kCancelled:
        state = BrokerState::kCancelled;
        break;
      case State::kRejected:
        state = BrokerState::kRejected;
        break;
      case State::kWorking:
      case State::kFree:
        break;
    }
    out.push_back({.state = state,
                   .filled_qty = r.filled_qty,
                   .average_price = r.average,
                   .broker_order_id = std::string(r.broker_order_id.view()),
                   .qty = r.qty,
                   .price = r.price,
                   .trigger_price = r.trigger});
  }
  return out;
}

// ---- the pump into the OMS ----

namespace {

[[nodiscard]] oms::EventDetails details_of(const SimEvent& e) noexcept {
  oms::EventDetails d;
  d.reason_code = e.reason_code;
  d.broker_order_id = e.broker_order_id.view();
  d.fill_qty = e.fill_qty;
  d.fill_price = e.fill_price;
  d.broker_trade_id = e.broker_trade_id.view();
  return d;
}

}  // namespace

namespace {

// What the pump does with one event, from the OMS's view of the order.
enum class Act : std::uint8_t { kApply, kUnknown, kMismatch, kWait, kStaleReject, kStaleConfirm };

struct Plan {
  Act act{Act::kApply};
  OrderEvent kind{OrderEvent::kBrokerAck};
};

[[nodiscard]] Plan plan_for(const SimEvent& e, const oms::Order& order) noexcept {
  using oms::OrderStatus;
  if (order.details.id != e.order_id) {
    return {Act::kMismatch, e.kind};
  }
  if (order.status == OrderStatus::kApproved) {  // handed over, SENT not recorded yet: wait for it
    return {Act::kWait, e.kind};
  }
  if (e.kind == OrderEvent::kCancelReject && order.status != OrderStatus::kCancelPending) {
    return {Act::kStaleReject, e.kind};  // the fill or the cancel came first, or no cancel was recorded
  }
  if (e.kind == OrderEvent::kCancelConfirm) {
    if (oms::is_terminal(order.status)) {
      return {Act::kStaleConfirm, e.kind};  // a repeated cancel of an order already cancelled
    }
    if (order.status == OrderStatus::kSent || order.status == OrderStatus::kOpen ||
        order.status == OrderStatus::kPartiallyFilled) {
      return {Act::kApply,
              OrderEvent::kBrokerCancel};  // no CANCEL_REQUEST was recorded: the broker cancelled
    }
  }
  return {Act::kApply, e.kind};
}

}  // namespace

std::size_t apply_events(SimBroker& broker, oms::Oms& oms) noexcept {
  std::size_t applied = 0;
  while (const SimEvent* e = broker.peek_event()) {
    const std::optional<oms::OrderHandle> handle = oms.find_handle(e->client_order_id.view());
    const Plan plan = handle.has_value() ? plan_for(*e, oms.order(*handle)) : Plan{Act::kUnknown, e->kind};
    if (plan.act == Act::kWait) {
      ++broker.stats_.apply_retries;  // keep the event; the router's next poll records SENT
      break;
    }
    if (plan.act == Act::kApply && handle.has_value()) {
      const auto result = oms.apply(*handle, plan.kind, details_of(*e));
      if (!result.has_value() && result.error().code == ErrorCode::kCapacityExceeded) {
        ++broker.stats_.apply_retries;  // keep the event: a fill is never dropped
        break;
      }
      if (result.has_value()) {
        ++applied;
      } else {
        ++broker.stats_.apply_errors;
      }
    } else if (plan.act == Act::kUnknown) {
      ++broker.stats_.unknown_orders;
    } else if (plan.act == Act::kMismatch) {
      ++broker.stats_.mismatched_orders;
    } else if (plan.act == Act::kStaleReject) {
      ++broker.stats_.stale_cancel_rejects;
    } else {
      ++broker.stats_.stale_cancel_confirms;
    }
    broker.discard_event();
  }
  return applied;
}

}  // namespace vyuha::sim

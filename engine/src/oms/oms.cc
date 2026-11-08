#include "vyuha/oms/oms.h"

#include <algorithm>
#include <array>
#include <bit>

#include "quill/LogMacros.h"
#include "vyuha/util/log.h"

namespace vyuha::oms {

namespace {

constexpr std::array<std::string_view, kOrderStatusCount> kStatusNames{
    "NEW",       "RISK_REJECTED",    "APPROVED",        "SENT",
    "OPEN",      "PARTIALLY_FILLED", "FILLED",          "CANCEL_PENDING",
    "CANCELLED", "MODIFY_PENDING",   "BROKER_REJECTED", "UNKNOWN",
    "EXPIRED",
};

constexpr Error kBadDetails{ErrorCode::kInvalidArgument, "oms: invalid event details"};
constexpr Error kSinkFull{ErrorCode::kCapacityExceeded, "oms: event sink full"};
constexpr Error kNoOrder{ErrorCode::kInvalidArgument, "oms: no such order"};

[[nodiscard]] constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

constexpr std::int64_t kMaxNumericUnits = 1'000'000'000'000'000'000;  // numeric(18,4)
constexpr std::int32_t kMaxMillipct = 1'000'000;                      // numeric(6,3)

// Postgres CHECKs and column ranges on `order`, so an order that can't be stored is refused here.
[[nodiscard]] bool same_request(const OrderDetails& a, const OrderDetails& b) noexcept {
  return a.client_order_id.view() == b.client_order_id.view() && a.parent_id == b.parent_id &&
         a.account_id == b.account_id && a.user_id == b.user_id && a.strategy_id == b.strategy_id &&
         a.source == b.source && a.instrument_id == b.instrument_id && a.side == b.side && a.qty == b.qty &&
         a.order_type == b.order_type && a.product == b.product && a.validity == b.validity &&
         a.price == b.price && a.trigger_price == b.trigger_price &&
         a.market_protection_millipct == b.market_protection_millipct && a.tag.view() == b.tag.view() &&
         a.is_paper == b.is_paper;
}

constexpr OrderHandle kEmptySlot = 0xFFFF'FFFF;
constexpr std::size_t kTradeSlotsPerOrder = 16;

[[nodiscard]] std::size_t fnv1a(std::string_view text) noexcept {
  constexpr std::uint64_t kBasis = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
  std::uint64_t hash = kBasis;
  for (const char c : text) {
    hash = (hash ^ static_cast<unsigned char>(c)) * kPrime;
  }
  return static_cast<std::size_t>(hash);
}

[[nodiscard]] std::size_t trade_hash(OrderHandle handle, std::string_view id) noexcept {
  constexpr std::uint64_t kMix = 0x9E37'79B9'7F4A'7C15ULL;
  return fnv1a(id) ^ static_cast<std::size_t>(std::uint64_t{handle} * kMix);
}

[[nodiscard]] bool modify_values_ok(const EventDetails& d) noexcept {
  const auto good_price = [](const std::optional<Price>& p) {
    return !p.has_value() || (p->units() > 0 && p->units() < kMaxNumericUnits);
  };
  return d.modified_qty >= 0 && good_price(d.modified_price) && good_price(d.modified_trigger);
}

// filled_qty after this event, or nullopt if the broker's total can't be true of it.
[[nodiscard]] std::optional<std::int32_t> filled_after(const Order& order, OrderEvent event,
                                                       std::int32_t broker) noexcept {
  const std::int32_t qty = order.details.qty;
  switch (event) {
    case OrderEvent::kReconcilePartial:
      return broker <= 0 || broker >= qty || broker < order.filled_qty ? std::nullopt : std::optional{broker};
    case OrderEvent::kReconcileCancelled:
      return broker >= qty || broker < order.filled_qty ? std::nullopt : std::optional{broker};
    case OrderEvent::kReconcileFilled:
      return qty;
    default:
      return order.filled_qty;
  }
}

[[nodiscard]] bool storable(const OrderDetails& d) noexcept {
  const bool needs_price = d.order_type == OrderType::kLimit || d.order_type == OrderType::kSl;
  const bool needs_trigger = d.order_type == OrderType::kSl || d.order_type == OrderType::kSlM;
  const auto fits = [](const std::optional<Price>& p) {
    return !p.has_value() || (p->units() > -kMaxNumericUnits && p->units() < kMaxNumericUnits);
  };
  const bool protection_fits =
      !d.market_protection_millipct.has_value() ||
      (*d.market_protection_millipct >= 0 && *d.market_protection_millipct < kMaxMillipct);
  return d.qty > 0 && !d.client_order_id.empty() && (!needs_price || d.price.has_value()) &&
         (!needs_trigger || d.trigger_price.has_value()) && fits(d.price) && fits(d.trigger_price) &&
         protection_fits;
}

}  // namespace

std::string_view to_string(OrderStatus s) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): every enumerator is in range
  return kStatusNames[static_cast<std::size_t>(s) - 1];
}

std::optional<OrderStatus> parse_status(std::string_view s) noexcept {
  for (std::size_t i = 0; i < kStatusNames.size(); ++i) {
    if (kStatusNames[i] == s) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index): < size()
      return static_cast<OrderStatus>(i + 1);
    }
  }
  return std::nullopt;
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): indices are bounded by the fixed text
// layout
std::optional<Uuid> Uuid::parse(std::string_view text) noexcept {
  constexpr std::size_t kTextSize = 36;
  if (text.size() != kTextSize) {
    return std::nullopt;
  }
  Uuid out;
  std::size_t byte = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') {
        return std::nullopt;
      }
      ++i;
      continue;
    }
    const int hi = hex_value(text[i]);
    const int lo = i + 1 < text.size() ? hex_value(text[i + 1]) : -1;
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.bytes[byte++] = static_cast<std::uint8_t>((hi << 4) | lo);
    i += 2;
  }
  return out;
}

std::array<char, Uuid::kTextSize> Uuid::to_chars() const noexcept {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::array<char, kTextSize> out{};
  std::size_t at = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[at++] = '-';
    }
    out[at++] = kHex[bytes[i] >> 4];
    out[at++] = kHex[bytes[i] & 0x0F];
  }
  return out;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

std::string Uuid::to_string() const {
  const auto text = to_chars();
  return {text.data(), text.size()};
}

// bugprone-exception-escape below: only the log macros can throw, and an engine that can't log should stop.
Oms::Oms(IOrderEventSink& sink, const IClock& clock, std::size_t capacity)
    : sink_(&sink), clock_(&clock), capacity_(capacity) {
  orders_.reserve(capacity);
  client_index_.assign(std::bit_ceil(std::max<std::size_t>(capacity, 1) * 2), kEmptySlot);
  trades_.resize(std::bit_ceil(std::max<std::size_t>(capacity, 1) * kTradeSlotsPerOrder));
}

bool Oms::trade_seen(OrderHandle handle, std::string_view id) const noexcept {
  const std::size_t mask = trades_.size() - 1;
  for (std::size_t slot = trade_hash(handle, id) & mask;; slot = (slot + 1) & mask) {
    const TradeSlot& entry = trades_[slot];
    if (entry.handle == kEmptySlot) {
      return false;
    }
    if (entry.handle == handle && entry.id.view() == id) {
      return true;
    }
  }
}

bool Oms::remember_trade(OrderHandle handle, std::string_view id) noexcept {
  // Three quarters full at most, so a lookup always ends at an empty slot, and soon.
  if ((trades_used_ + 1) * 4 > trades_.size() * 3) {
    return false;
  }
  const std::size_t mask = trades_.size() - 1;
  std::size_t slot = trade_hash(handle, id) & mask;
  while (trades_[slot].handle != kEmptySlot) {
    slot = (slot + 1) & mask;
  }
  if (!trades_[slot].id.assign(id)) {
    return false;
  }
  trades_[slot].handle = handle;
  ++trades_used_;
  return true;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<Intake, Error> Oms::intake(const OrderDetails& details) noexcept {
  if (!storable(details)) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "oms: order can't be stored"});
  }
  if (const auto existing = find_client(details.client_order_id.view()); existing.has_value()) {
    if (!same_request(orders_[*existing].details, details)) {
      return std::unexpected(Error{ErrorCode::kOrderDup, "ORDER_DUP"});
    }
    return Intake{.handle = *existing, .existing = true};
  }
  // Once per new order, not per tick, and the pool holds a day's orders: a scan is cheap enough.
  if (std::ranges::any_of(orders_, [&](const Order& o) { return o.details.id == details.id; })) {
    return std::unexpected(Error{ErrorCode::kOrderDup, "ORDER_DUP: order id already used"});
  }
  if (orders_.size() >= capacity_) {
    return std::unexpected(Error{ErrorCode::kCapacityExceeded, "oms: order pool full"});
  }
  OrderEventRecord event;
  event.order_id = details.id;
  event.seq = 1;
  event.to = OrderStatus::kNew;
  event.ts = clock_->now();
  event.details = details;
  if (!sink_->try_push(event)) {
    return std::unexpected(kSinkFull);
  }
  Order& order = orders_.emplace_back();  // can't allocate: within the reserved capacity
  order.details = details;
  order.seq = 1;
  const auto handle = static_cast<OrderHandle>(orders_.size() - 1);
  index_client(handle);
  return Intake{.handle = handle, .existing = false};
}

std::expected<OrderHandle, Error> Oms::create(const OrderDetails& details) noexcept {
  const auto taken = intake(details);
  if (!taken.has_value()) {
    return std::unexpected(taken.error());
  }
  return taken->handle;
}

std::optional<OrderHandle> Oms::find_client(std::string_view client_order_id) const noexcept {
  const std::size_t mask = client_index_.size() - 1;
  for (std::size_t slot = fnv1a(client_order_id) & mask;; slot = (slot + 1) & mask) {
    const OrderHandle handle = client_index_[slot];
    if (handle == kEmptySlot) {
      return std::nullopt;
    }
    if (orders_[handle].details.client_order_id.view() == client_order_id) {
      return handle;
    }
  }
}

void Oms::index_client(OrderHandle handle) noexcept {
  const std::size_t mask = client_index_.size() - 1;
  std::size_t slot = fnv1a(orders_[handle].details.client_order_id.view()) & mask;
  while (client_index_[slot] != kEmptySlot) {
    slot = (slot + 1) & mask;
  }
  client_index_[slot] = handle;
}

void Oms::refuse(const Uuid& order_id) noexcept {
  for (Order& order : orders_) {  // rare, and not per tick
    if (order.details.id == order_id) {
      ++refused_;
      if (order.status == OrderStatus::kNew || order.status == OrderStatus::kApproved) {
        order.status = OrderStatus::kRiskRejected;
      }
      return;
    }
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::illegal(const Order& order, OrderEvent event) noexcept {
  ++illegal_;
  if (quill::Logger* logger = log::engine(); logger != nullptr) {
    const auto id = order.details.id.to_chars();
    LOG_ERROR(logger, "OMS_ILLEGAL_TRANSITION order={} status={} event={}",
              std::string_view(id.data(), id.size()), to_string(order.status), static_cast<int>(event));
  }
  return std::unexpected(Error{ErrorCode::kIllegalTransition, "OMS_ILLEGAL_TRANSITION"});
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::record(Order& order, OrderStatus to, std::int32_t filled,
                                              const EventDetails& details, std::string_view reason,
                                              const ModifyRecord& modify) noexcept {
  OrderEventRecord event;
  FixedString<40> broker_id = order.broker_order_id;
  if (!event.reason_code.assign(reason) ||
      (!details.broker_order_id.empty() && !broker_id.assign(details.broker_order_id))) {
    return std::unexpected(kBadDetails);
  }
  event.order_id = order.details.id;
  event.seq = order.seq + 1;
  event.from = order.status;
  event.to = to;
  event.filled_qty = filled;
  event.fill_qty = details.fill_qty;
  event.fill_price = details.fill_price;
  event.broker_order_id = broker_id;
  event.modify = modify;
  event.gate = details.gate;
  // Too long to store is the caller's mismatch to raise; the fill itself must still go in.
  if (!event.broker_trade_id.assign(details.broker_trade_id)) {
    event.broker_trade_id = {};
  }
  event.ts = clock_->now();
  if (!sink_->try_push(event)) {
    return std::unexpected(kSinkFull);
  }
  order.status = to;
  order.seq = event.seq;
  order.filled_qty = filled;
  order.broker_order_id = broker_id;
  return order.status;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::fill(Order& order, const EventDetails& details) noexcept {
  if (details.fill_qty <= 0) {
    return std::unexpected(kBadDetails);
  }
  const auto handle = static_cast<OrderHandle>(&order - orders_.data());
  const std::string_view trade = details.broker_trade_id;
  const bool has_trade = !trade.empty();
  const bool trade_fits = trade.size() <= FixedString<40>::capacity();
  if (has_trade && trade_fits && trade_seen(handle, trade)) {
    const auto recorded =
        record(order, order.status, order.filled_qty,
               {.broker_order_id = details.broker_order_id, .broker_trade_id = trade}, kDuplicateFill);
    if (recorded.has_value()) {
      ++duplicate_fills_;
    }
    return recorded;
  }
  const std::int64_t total = std::int64_t{order.filled_qty} + details.fill_qty;
  const std::int32_t qty = order.details.qty;
  // With a modify out, qty is the higher of the two: a lower one may yet be refused (Order::modify_out).
  const OrderEvent event = total >= qty ? OrderEvent::kFill : OrderEvent::kPartialFill;
  const std::optional<OrderStatus> next = next_status(order.status, event);
  // The room is checked first: a trade that can't be remembered could be counted twice, so it is raised.
  const bool trade_room = !has_trade || (trade_fits && (trades_used_ + 1) * 4 <= trades_.size() * 3);
  const bool mismatch = !next.has_value() || total > qty || details.fill_price.units() <= 0 || !trade_room;
  const auto filled = static_cast<std::int32_t>(std::min<std::int64_t>(total, qty));
  const auto recorded = record(order, next.value_or(order.status), filled, details,
                               mismatch ? kReconMismatch : std::string_view{});
  // Only now: a fill the sink refused was never counted, so its retry mustn't look like a duplicate.
  if (recorded.has_value() && has_trade && trade_room) {
    static_cast<void>(remember_trade(handle, trade));
  }
  if (recorded.has_value() && mismatch) {
    ++recon_mismatches_;
    if (quill::Logger* logger = log::engine(); logger != nullptr) {
      const auto id = order.details.id.to_chars();
      LOG_ERROR(logger, "RECON_MISMATCH fill order={} status={} filled={} qty={} fill={}",
                std::string_view(id.data(), id.size()), to_string(order.status), order.filled_qty, qty,
                details.fill_qty);
    }
  }
  return recorded;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::apply(OrderHandle handle, OrderEvent event,
                                             const EventDetails& details) noexcept {
  if (handle >= orders_.size()) {
    return std::unexpected(kNoOrder);
  }
  Order& order = orders_[handle];
  if (event == OrderEvent::kPartialFill || event == OrderEvent::kFill) {
    return fill(order, details);
  }
  const bool modify_answer = event == OrderEvent::kModifyConfirm || event == OrderEvent::kModifyReject;
  // An answer to a modify still out, after a fill moved the order on: the answer is still a fact.
  const bool late_answer = modify_answer && order.modify_out && order.status != OrderStatus::kModifyPending;
  const std::optional<OrderStatus> next =
      late_answer ? std::optional{order.status} : next_status(order.status, event);
  if (!next.has_value() ||
      (order.needs_reconcile && (event == OrderEvent::kSend || event == OrderEvent::kModifyRequest))) {
    return illegal(order, event);
  }
  if (event == OrderEvent::kModifyRequest || modify_answer) {
    return apply_modify(order, event, *next, details);
  }
  if (event == OrderEvent::kSend && !sink_->healthy()) {
    return std::unexpected(Error{ErrorCode::kOrderLogUnavailable, "oms: order events aren't being written"});
  }
  const bool is_rejection = event == OrderEvent::kRiskReject || event == OrderEvent::kBrokerReject ||
                            event == OrderEvent::kReconcileRejected || event == OrderEvent::kExpire;
  if (is_rejection && details.reason_code.empty()) {
    return std::unexpected(kBadDetails);
  }
  const std::optional<std::int32_t> filled = filled_after(order, event, details.broker_filled_qty);
  if (!filled.has_value()) {
    return std::unexpected(kBadDetails);
  }

  // What reconciliation learned is a fill too: the event says how much, for the position projector.
  EventDetails recorded = details;
  recorded.fill_qty = *filled - order.filled_qty;
  if (recorded.fill_qty == 0) {
    recorded.fill_price = Price{};
  }
  const bool late_ack = event == OrderEvent::kBrokerAck && *next == order.status;
  const auto result = record(order, *next, *filled, recorded, late_ack ? kLateAck : details.reason_code);
  if (result.has_value() && event >= OrderEvent::kReconcileOpen) {
    order.needs_reconcile = false;
  }
  return result;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::apply_modify(Order& order, OrderEvent event, OrderStatus next,
                                                    const EventDetails& details) noexcept {
  if (!modify_values_ok(details)) {
    return std::unexpected(kBadDetails);
  }
  return event == OrderEvent::kModifyRequest ? modify_request(order, next, details)
                                             : modify_answer(order, event, next, details);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::modify_request(Order& order, OrderStatus next,
                                                      const EventDetails& details) noexcept {
  if (details.modified_qty != 0 && details.modified_qty <= order.filled_qty) {
    return std::unexpected(kBadDetails);  // nothing would be left to work: that's a cancel
  }
  if (order.modify_out) {
    return std::unexpected(kBadDetails);  // one at a time: a second would lose what to go back to
  }
  OrderDetails& live = order.details;
  ModifyRecord modify{
      .qty = details.modified_qty, .price = details.modified_price, .trigger = details.modified_trigger};
  const bool asks = modify.qty != 0 || modify.price.has_value() || modify.trigger.has_value();
  const std::int32_t raised = std::max(live.qty, modify.qty);
  if (raised != live.qty) {
    modify.row_qty = raised;  // fills against the new qty can arrive before the confirm
  }
  const auto recorded = record(order, next, order.filled_qty, details, {}, modify);
  if (recorded.has_value() && asks) {
    order.modify_out = true;
    order.qty_before_modify = live.qty;
    order.modify_qty = modify.qty;
    order.modify_price = modify.price;
    order.modify_trigger = modify.trigger;
    live.qty = raised;
  }
  return recorded;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::modify_answer(Order& order, OrderEvent event, OrderStatus next,
                                                     const EventDetails& details) noexcept {
  OrderDetails& live = order.details;
  const bool confirmed = event == OrderEvent::kModifyConfirm;
  ModifyRecord modify;
  if (confirmed) {
    modify.qty = details.modified_qty != 0 ? details.modified_qty : order.modify_qty;
    modify.price = details.modified_price.has_value() ? details.modified_price : order.modify_price;
    modify.trigger = details.modified_trigger.has_value() ? details.modified_trigger : order.modify_trigger;
    modify.applied = true;
  }
  std::int32_t answered = live.qty;
  if (confirmed && modify.qty != 0) {
    answered = modify.qty;
  } else if (!confirmed && order.modify_out) {
    answered = order.qty_before_modify;
  }
  const bool below_fills = answered < order.filled_qty;  // the broker confirmed less than has already filled
  const std::int32_t new_qty = std::max(answered, order.filled_qty);  // the row's CHECK can't take less
  if (new_qty != live.qty || (confirmed && modify.qty != 0)) {
    modify.row_qty = new_qty;
  }
  // The answer is what completed it.
  const OrderStatus to = !is_terminal(next) && order.filled_qty >= new_qty ? OrderStatus::kFilled : next;
  const auto recorded =
      record(order, to, order.filled_qty, details, below_fills ? kReconMismatch : std::string_view{}, modify);
  if (recorded.has_value()) {
    recon_mismatches_ += below_fills ? 1U : 0U;
    live.qty = new_qty;
    live.price = modify.price.has_value() ? modify.price : live.price;
    live.trigger_price = modify.trigger.has_value() ? modify.trigger : live.trigger_price;
    order.modify_out = false;
    order.modify_qty = 0;
    order.modify_price.reset();
    order.modify_trigger.reset();
  }
  return recorded;
}

bool Oms::can_send(OrderHandle handle) const noexcept {
  if (handle >= orders_.size()) {
    return false;
  }
  const Order& order = orders_[handle];
  return order.status == OrderStatus::kApproved && !order.needs_reconcile && sink_->healthy();
}

std::expected<OrderStatus, Error> Oms::record_sent(OrderHandle handle) noexcept {
  if (handle >= orders_.size() || orders_[handle].status != OrderStatus::kApproved) {
    return std::unexpected(kNoOrder);
  }
  return record(orders_[handle], OrderStatus::kSent, orders_[handle].filled_qty, {}, {});
}

std::expected<void, Error> Oms::raise_mismatch(OrderHandle handle) noexcept {
  if (handle >= orders_.size()) {
    return std::unexpected(kNoOrder);
  }
  Order& order = orders_[handle];
  const auto recorded = record(order, order.status, order.filled_qty, {}, kReconMismatch);
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  ++recon_mismatches_;
  return {};
}

std::expected<void, Error> Oms::confirm_reconciled(OrderHandle handle) noexcept {
  if (handle >= orders_.size()) {
    return std::unexpected(kNoOrder);
  }
  const OrderStatus status = orders_[handle].status;
  if (status == OrderStatus::kNew || status == OrderStatus::kApproved) {
    return std::unexpected(
        Error{ErrorCode::kInvalidArgument, "oms: a restored unsent order is never cleared"});
  }
  orders_[handle].needs_reconcile = false;
  return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderHandle, Error> Oms::restore(const RecoveredOrder& recovered) noexcept {
  if (orders_.size() >= capacity_) {
    return std::unexpected(Error{ErrorCode::kCapacityExceeded, "oms: order pool full"});
  }
  FixedString<40> broker_id;
  if (is_terminal(recovered.status) || recovered.seq < 1 || !broker_id.assign(recovered.broker_order_id)) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "oms: order can't be restored"});
  }
  Order& order = orders_.emplace_back();
  order.details = recovered.details;
  order.status = recovered.status;
  order.seq = recovered.seq;
  order.filled_qty = recovered.filled_qty;
  order.broker_order_id = broker_id;
  order.needs_reconcile = true;
  const auto handle = static_cast<OrderHandle>(orders_.size() - 1);
  index_client(handle);
  for (const std::string& trade : recovered.trade_ids) {
    if (!trade_seen(handle, trade)) {
      static_cast<void>(remember_trade(handle, trade));  // no room: the next fill raises a mismatch
    }
  }
  return handle;
}

}  // namespace vyuha::oms

#include "vyuha/oms/oms.h"

#include <algorithm>
#include <array>

#include "quill/LogMacros.h"
#include "vyuha/util/log.h"

namespace vyuha::oms {

namespace {

constexpr std::array<std::string_view, kOrderStatusCount> kStatusNames{
    "NEW",       "RISK_REJECTED",    "APPROVED",        "SENT",
    "OPEN",      "PARTIALLY_FILLED", "FILLED",          "CANCEL_PENDING",
    "CANCELLED", "MODIFY_PENDING",   "BROKER_REJECTED", "UNKNOWN",
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
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderHandle, Error> Oms::create(const OrderDetails& details) noexcept {
  if (!storable(details)) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "oms: order can't be stored"});
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
  return static_cast<OrderHandle>(orders_.size() - 1);
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
                                              const EventDetails& details, std::string_view reason) noexcept {
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
  const std::int64_t total = std::int64_t{order.filled_qty} + details.fill_qty;
  const std::int32_t qty = order.details.qty;
  const OrderEvent event = total >= qty ? OrderEvent::kFill : OrderEvent::kPartialFill;
  const std::optional<OrderStatus> next = next_status(order.status, event);
  const bool mismatch = !next.has_value() || total > qty || details.fill_price.units() <= 0;
  const auto filled = static_cast<std::int32_t>(std::min<std::int64_t>(total, qty));
  const auto recorded = record(order, next.value_or(order.status), filled, details,
                               mismatch ? kReconMismatch : std::string_view{});
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
  const std::optional<OrderStatus> next = next_status(order.status, event);
  if (!next.has_value() ||
      (order.needs_reconcile && (event == OrderEvent::kSend || event == OrderEvent::kModifyRequest))) {
    return illegal(order, event);
  }
  if (event == OrderEvent::kSend && !sink_->healthy()) {
    return std::unexpected(Error{ErrorCode::kOrderLogUnavailable, "oms: order events aren't being written"});
  }
  const bool is_rejection = event == OrderEvent::kRiskReject || event == OrderEvent::kBrokerReject ||
                            event == OrderEvent::kReconcileRejected;
  if (is_rejection && details.reason_code.empty()) {
    return std::unexpected(kBadDetails);
  }

  const std::int32_t qty = order.details.qty;
  const std::int32_t broker = details.broker_filled_qty;
  std::int32_t filled = order.filled_qty;
  switch (event) {
    case OrderEvent::kReconcilePartial:
      if (broker <= 0 || broker >= qty || broker < order.filled_qty) {
        return std::unexpected(kBadDetails);
      }
      filled = broker;
      break;
    case OrderEvent::kReconcileCancelled:
      if (broker >= qty || broker < order.filled_qty) {
        return std::unexpected(kBadDetails);
      }
      filled = broker;
      break;
    case OrderEvent::kReconcileFilled:
      filled = qty;
      break;
    default:
      break;
  }

  // What reconciliation learned is a fill too: the event says how much, for the position projector.
  EventDetails recorded = details;
  recorded.fill_qty = filled - order.filled_qty;
  if (recorded.fill_qty == 0) {
    recorded.fill_price = Price{};
  }
  const auto result = record(order, *next, filled, recorded, details.reason_code);
  if (result.has_value() && event >= OrderEvent::kReconcileOpen) {
    order.needs_reconcile = false;
  }
  return result;
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
  return static_cast<OrderHandle>(orders_.size() - 1);
}

}  // namespace vyuha::oms

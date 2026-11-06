#include "vyuha/oms/oms.h"

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
std::expected<std::int32_t, Error> Oms::filled_after(const Order& order, OrderEvent event,
                                                     const EventDetails& details) noexcept {
  const std::int32_t qty = order.details.qty;
  const std::int32_t broker = details.broker_filled_qty;
  switch (event) {
    case OrderEvent::kPartialFill:
    case OrderEvent::kFill: {
      const std::int32_t left = qty - order.filled_qty;
      const bool fits = event == OrderEvent::kFill ? details.fill_qty == left : details.fill_qty < left;
      if (details.fill_qty <= 0 || details.fill_price.units() <= 0 || !fits) {
        ++refused_fills_;
        if (quill::Logger* logger = log::engine(); logger != nullptr) {
          const auto id = order.details.id.to_chars();
          LOG_ERROR(logger, "OMS_FILL_REFUSED order={} filled={} qty={} fill={}",
                    std::string_view(id.data(), id.size()), order.filled_qty, qty, details.fill_qty);
        }
        return std::unexpected(kBadDetails);
      }
      return order.filled_qty + details.fill_qty;
    }
    case OrderEvent::kReconcilePartial:
      if (broker <= 0 || broker >= qty || broker < order.filled_qty) {
        return std::unexpected(kBadDetails);
      }
      return broker;
    case OrderEvent::kReconcileCancelled:
      if (broker >= qty || broker < order.filled_qty) {
        return std::unexpected(kBadDetails);
      }
      return broker;
    case OrderEvent::kReconcileFilled:
      return qty;
    default:
      return order.filled_qty;
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::expected<OrderStatus, Error> Oms::apply(OrderHandle handle, OrderEvent event,
                                             const EventDetails& details) noexcept {
  if (handle >= orders_.size()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "oms: no such order"});
  }
  Order& order = orders_[handle];
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

  const auto filled = filled_after(order, event, details);
  if (!filled.has_value()) {
    return std::unexpected(filled.error());
  }

  OrderEventRecord record;
  FixedString<40> broker_id = order.broker_order_id;
  if (!record.reason_code.assign(details.reason_code) ||
      (!details.broker_order_id.empty() && !broker_id.assign(details.broker_order_id))) {
    return std::unexpected(kBadDetails);
  }
  record.order_id = order.details.id;
  record.seq = order.seq + 1;
  record.from = order.status;
  record.to = *next;
  record.filled_qty = *filled;
  if (event == OrderEvent::kPartialFill || event == OrderEvent::kFill) {
    record.fill_qty = details.fill_qty;
    record.fill_price = details.fill_price;
  }
  record.broker_order_id = broker_id;
  record.ts = clock_->now();
  if (!sink_->try_push(record)) {
    return std::unexpected(kSinkFull);
  }
  order.status = *next;
  order.seq = record.seq;
  order.filled_qty = *filled;
  order.broker_order_id = broker_id;
  if (event >= OrderEvent::kReconcileOpen) {
    order.needs_reconcile = false;
  }
  return order.status;
}

std::expected<void, Error> Oms::confirm_reconciled(OrderHandle handle) noexcept {
  if (handle >= orders_.size()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "oms: no such order"});
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

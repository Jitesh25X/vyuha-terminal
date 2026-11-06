#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vyuha::oms {

// Values match orders.v1.OrderStatus; names match the Postgres order_status enum.
enum class OrderStatus : std::uint8_t {
  kNew = 1,
  kRiskRejected,
  kApproved,
  kSent,
  kOpen,
  kPartiallyFilled,
  kFilled,
  kCancelPending,
  kCancelled,
  kModifyPending,
  kBrokerRejected,
  kUnknown,
};
inline constexpr std::uint8_t kOrderStatusCount = 12;

enum class OrderEvent : std::uint8_t {
  kRiskReject,
  kRiskApprove,
  kSend,
  kBrokerAck,
  kBrokerReject,
  kPartialFill,
  kFill,  // the order is now complete
  kCancelRequest,
  kCancelConfirm,
  kModifyRequest,  // after the modify passed the risk check
  kModifyConfirm,
  kAckTimeout,  // 5 s after Send without an ack
  // Reconcile: what the broker's order book says about an UNKNOWN order.
  kReconcileOpen,
  kReconcilePartial,
  kReconcileFilled,
  kReconcileCancelled,
  kReconcileRejected,
};
inline constexpr std::uint8_t kOrderEventCount = 17;

// The F-EX-2 table. nullopt means the transition isn't listed, so it is illegal (OMS_ILLEGAL_TRANSITION).
// There is no way from UNKNOWN to APPROVED or SENT: an UNKNOWN order is never re-sent.
[[nodiscard]] constexpr std::optional<OrderStatus> next_status(OrderStatus from, OrderEvent event) noexcept {
  using enum OrderEvent;
  switch (from) {
    case OrderStatus::kNew:
      if (event == kRiskReject) {
        return OrderStatus::kRiskRejected;
      }
      if (event == kRiskApprove) {
        return OrderStatus::kApproved;
      }
      return std::nullopt;
    case OrderStatus::kApproved:
      return event == kSend ? std::optional{OrderStatus::kSent} : std::nullopt;
    case OrderStatus::kSent:
      if (event == kBrokerAck) {
        return OrderStatus::kOpen;
      }
      if (event == kBrokerReject) {
        return OrderStatus::kBrokerRejected;
      }
      return event == kAckTimeout ? std::optional{OrderStatus::kUnknown} : std::nullopt;
    case OrderStatus::kOpen:
    case OrderStatus::kPartiallyFilled:
      switch (event) {
        case kPartialFill:  // a second partial fill stays PARTIALLY_FILLED
          return OrderStatus::kPartiallyFilled;
        case kFill:
          return OrderStatus::kFilled;
        case kCancelRequest:
          return OrderStatus::kCancelPending;
        case kModifyRequest:
          return OrderStatus::kModifyPending;
        default:
          return std::nullopt;
      }
    case OrderStatus::kCancelPending:
      return event == kCancelConfirm ? std::optional{OrderStatus::kCancelled} : std::nullopt;
    case OrderStatus::kModifyPending:
      return event == kModifyConfirm ? std::optional{OrderStatus::kOpen} : std::nullopt;
    case OrderStatus::kUnknown:
      switch (event) {
        case kReconcileOpen:
          return OrderStatus::kOpen;
        case kReconcilePartial:
          return OrderStatus::kPartiallyFilled;
        case kReconcileFilled:
          return OrderStatus::kFilled;
        case kReconcileCancelled:
          return OrderStatus::kCancelled;
        case kReconcileRejected:
          return OrderStatus::kBrokerRejected;
        default:
          return std::nullopt;
      }
    case OrderStatus::kRiskRejected:
    case OrderStatus::kFilled:
    case OrderStatus::kCancelled:
    case OrderStatus::kBrokerRejected:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
  return s == OrderStatus::kRiskRejected || s == OrderStatus::kFilled || s == OrderStatus::kCancelled ||
         s == OrderStatus::kBrokerRejected;
}

[[nodiscard]] std::string_view to_string(OrderStatus s) noexcept;
[[nodiscard]] std::optional<OrderStatus> parse_status(std::string_view s) noexcept;

}  // namespace vyuha::oms

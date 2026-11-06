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
  kExpired,  // APPROVED, never reached the broker, and the engine restarted: it never fires
};
inline constexpr std::uint8_t kOrderStatusCount = 13;

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
  kCancelReject,  // the broker refused the cancel
  kModifyReject,  // the broker refused the modify
  kBrokerCancel,  // broker- or exchange-initiated: IOC remainder, MIS auto square-off, exchange cancel
  kAckTimeout,    // 5 s after Send without an ack
  kExpire,        // the restart rule (ADR-025): recovery found an APPROVED order the broker doesn't have
  // Reconcile: what the broker's order book says about an UNKNOWN order. Keep these last: Oms::apply treats
  // anything from kReconcileOpen on as settling the order against the broker.
  kReconcileOpen,
  kReconcilePartial,
  kReconcileFilled,
  kReconcileCancelled,
  kReconcileRejected,
};
inline constexpr std::uint8_t kOrderEventCount = 21;

// The F-EX-2 table (ADR-024). nullopt means the transition isn't listed, so it is illegal
// (OMS_ILLEGAL_TRANSITION), except that a fill is a broker fact and Oms records it anyway (RECON_MISMATCH).
// There is no way from UNKNOWN to APPROVED or SENT: an UNKNOWN order is never re-sent.
[[nodiscard]] constexpr std::optional<OrderStatus> next_status(OrderStatus from, OrderEvent event) noexcept {
  using enum OrderEvent;
  // A late ack: the fill or cancel beat it. Recorded, and the status stays.
  if (event == kBrokerAck && (from == OrderStatus::kPartiallyFilled || from == OrderStatus::kFilled ||
                              from == OrderStatus::kCancelled || from == OrderStatus::kCancelPending ||
                              from == OrderStatus::kModifyPending)) {
    return from;
  }
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
      if (event == kExpire) {
        return OrderStatus::kExpired;
      }
      return event == kSend ? std::optional{OrderStatus::kSent} : std::nullopt;
    case OrderStatus::kSent:
      switch (event) {
        case kBrokerAck:
          return OrderStatus::kOpen;
        case kBrokerReject:
          return OrderStatus::kBrokerRejected;
        case kAckTimeout:
          return OrderStatus::kUnknown;
        case kPartialFill:  // a fill can beat the ack
          return OrderStatus::kPartiallyFilled;
        case kFill:
          return OrderStatus::kFilled;
        case kBrokerCancel:  // an IOC cancelled before the ack
          return OrderStatus::kCancelled;
        default:
          return std::nullopt;
      }
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
        case kBrokerCancel:
          return OrderStatus::kCancelled;
        case kBrokerReject:  // a post-ack RMS reject; only from OPEN: a partly filled order has traded
          return from == OrderStatus::kOpen ? std::optional{OrderStatus::kBrokerRejected} : std::nullopt;
        default:
          return std::nullopt;
      }
    case OrderStatus::kCancelPending:
      switch (event) {
        case kPartialFill:  // the fill raced the cancel
          return OrderStatus::kPartiallyFilled;
        case kFill:
          return OrderStatus::kFilled;
        case kCancelConfirm:
          return OrderStatus::kCancelled;
        case kCancelReject:
          return OrderStatus::kOpen;
        default:
          return std::nullopt;
      }
    case OrderStatus::kModifyPending:
      switch (event) {
        case kPartialFill:  // a fill during the modify
          return OrderStatus::kPartiallyFilled;
        case kFill:
          return OrderStatus::kFilled;
        case kModifyConfirm:
        case kModifyReject:
          return OrderStatus::kOpen;
        default:
          return std::nullopt;
      }
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
    case OrderStatus::kExpired:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
  return s == OrderStatus::kRiskRejected || s == OrderStatus::kFilled || s == OrderStatus::kCancelled ||
         s == OrderStatus::kBrokerRejected || s == OrderStatus::kExpired;
}

[[nodiscard]] std::string_view to_string(OrderStatus s) noexcept;
[[nodiscard]] std::optional<OrderStatus> parse_status(std::string_view s) noexcept;

}  // namespace vyuha::oms

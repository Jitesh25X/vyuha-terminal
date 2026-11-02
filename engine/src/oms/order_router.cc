#include "vyuha/oms/order_router.h"

#include <cctype>

#include "quill/LogMacros.h"
#include "vyuha/util/log.h"

namespace vyuha::oms {

namespace {

constexpr std::string_view kNeverApproved = "RESTARTED_BEFORE_APPROVAL";
constexpr std::string_view kBrokerRejected = "BROKER_REJECTED";

}  // namespace

std::string broker_tag(std::string_view client_order_id, std::size_t limit) {
  std::string out;
  out.reserve(client_order_id.size());
  for (const char c : client_order_id) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      out += c;
    }
  }
  return out.size() > limit ? out.substr(out.size() - limit) : out;
}

OrderRouter::OrderRouter(Oms& oms, IOrderEventSink& sink, IOrderSender& sender, std::size_t tag_limit,
                         std::size_t capacity)
    : oms_(&oms), sink_(&sink), sender_(&sender), tag_limit_(tag_limit), capacity_(capacity) {
  pending_.reserve(capacity);
}

// NOLINTNEXTLINE(bugprone-exception-escape): within the reserved capacity, so push_back can't allocate
std::expected<void, Error> OrderRouter::submit(OrderHandle handle) noexcept {
  if (pending() >= capacity_ || pending_.size() >= capacity_) {
    return std::unexpected(Error{ErrorCode::kCapacityExceeded, "router: queue full"});
  }
  if (!oms_->can_send(handle)) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "router: order can't be sent"});
  }
  pending_.push_back(Pending{.handle = handle, .ticket = sink_->pushed(), .sent = false});
  return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape): the tag fits small-string storage or the engine stops
std::size_t OrderRouter::poll() noexcept {
  std::size_t sent = 0;
  while (head_ < pending_.size()) {
    Pending& next = pending_[head_];
    if (!next.sent) {
      if (sink_->durable() < next.ticket || !sink_->healthy()) {
        break;
      }
      const Order& order = oms_->order(next.handle);
      if (!oms_->can_send(next.handle)) {  // cancelled or otherwise moved on while it waited
        ++head_;
        continue;
      }
      // The tag is rebuilt here, not stored per order; a client id is short and this is once per order.
      const std::string tag = broker_tag(order.details.client_order_id.view(), tag_limit_);
      if (!sender_->send(order, tag)) {
        break;
      }
      next.sent = true;
      ++sent;
    }
    if (!oms_->record_sent(next.handle).has_value()) {
      break;  // the sink is full: try again on the next poll, and never send again
    }
    ++head_;
  }
  if (head_ == pending_.size()) {
    pending_.clear();
    head_ = 0;
  }
  return sent;
}

OrderRouter::Recovery OrderRouter::unresolved(OrderHandle handle) {
  if (!oms_->raise_mismatch(handle).has_value()) {
    if (quill::Logger* logger = log::engine(); logger != nullptr) {
      LOG_ERROR(logger, "RECON_MISMATCH couldn't be recorded: the event sink is full");
    }
  }
  return Recovery::kUnresolved;
}

OrderRouter::Recovery OrderRouter::reconcile_unknown(OrderHandle handle, const BrokerOrderView& broker) {
  using State = BrokerOrderView::State;
  EventDetails details{.broker_order_id = broker.broker_order_id,
                       .fill_price = broker.average_price,
                       .broker_filled_qty = broker.filled_qty};
  OrderEvent event = OrderEvent::kReconcileOpen;
  switch (broker.state) {
    case State::kOpen:
      event = OrderEvent::kReconcileOpen;
      break;
    case State::kPartiallyFilled:
      event = OrderEvent::kReconcilePartial;
      break;
    case State::kFilled:
      event = OrderEvent::kReconcileFilled;
      break;
    case State::kCancelled:
      event = OrderEvent::kReconcileCancelled;
      break;
    case State::kRejected:
      event = OrderEvent::kReconcileRejected;
      details.reason_code = kBrokerRejected;
      break;
  }
  return oms_->apply(handle, event, details).has_value() ? Recovery::kResolved : unresolved(handle);
}

OrderRouter::Recovery OrderRouter::catch_up(OrderHandle handle, const BrokerOrderView& broker) {
  using State = BrokerOrderView::State;
  const std::int32_t missed = broker.filled_qty - oms_->order(handle).filled_qty;
  if (missed < 0) {
    return unresolved(handle);  // we hold more fills than the broker does
  }
  if (missed > 0 && !oms_->apply(handle, OrderEvent::kFill,
                                 {.broker_order_id = broker.broker_order_id,
                                  .fill_qty = missed,
                                  .fill_price = broker.average_price})
                         .has_value()) {
    return unresolved(handle);
  }
  const OrderStatus status = oms_->order(handle).status;
  bool settled = true;
  if (broker.state == State::kCancelled) {
    if (status == OrderStatus::kCancelPending) {
      settled = oms_->apply(handle, OrderEvent::kCancelConfirm).has_value();
    } else if (status == OrderStatus::kModifyPending) {
      settled = oms_->apply(handle, OrderEvent::kModifyReject).has_value() &&
                oms_->apply(handle, OrderEvent::kBrokerCancel).has_value();
    } else {
      settled = oms_->apply(handle, OrderEvent::kBrokerCancel).has_value();
    }
  } else if (broker.state == State::kRejected) {
    settled = oms_->apply(handle, OrderEvent::kBrokerReject, {.reason_code = kBrokerRejected}).has_value();
  } else if (status == OrderStatus::kCancelPending) {
    settled = oms_->apply(handle, OrderEvent::kCancelReject).has_value();
  } else if (status == OrderStatus::kModifyPending) {
    settled = oms_->apply(handle, OrderEvent::kModifyConfirm).has_value();
  }
  if (!settled) {
    return unresolved(handle);
  }
  return oms_->confirm_reconciled(handle).has_value() ? Recovery::kResolved : Recovery::kUnresolved;
}

OrderRouter::Recovery OrderRouter::recover(OrderHandle handle, IBrokerOrderBook& book) {
  const Order& order = oms_->order(handle);
  const std::vector<BrokerOrderView> found =
      book.find_by_tag(broker_tag(order.details.client_order_id.view(), tag_limit_));
  if (found.size() > 1) {
    return unresolved(handle);  // never guess which one is ours
  }
  const BrokerOrderView* broker = found.empty() ? nullptr : found.data();
  switch (order.status) {
    case OrderStatus::kNew:
      if (broker != nullptr) {  // it can't have been sent: APPROVED was never durable
        return unresolved(handle);
      }
      return oms_->apply(handle, OrderEvent::kRiskReject, {.reason_code = kNeverApproved}).has_value()
                 ? Recovery::kResolved
                 : Recovery::kUnresolved;
    case OrderStatus::kApproved:
      if (!oms_->confirm_reconciled(handle).has_value()) {
        return Recovery::kUnresolved;
      }
      if (broker == nullptr) {  // never reached the broker: this is its first send, not a re-send
        return submit(handle).has_value() ? Recovery::kQueued : Recovery::kUnresolved;
      }
      if (!oms_->record_sent(handle).has_value() ||
          !oms_->apply(handle, OrderEvent::kAckTimeout).has_value()) {
        return Recovery::kUnresolved;
      }
      return reconcile_unknown(handle, *broker);
    case OrderStatus::kSent:
      if (!oms_->apply(handle, OrderEvent::kAckTimeout).has_value()) {
        return Recovery::kUnresolved;
      }
      [[fallthrough]];
    case OrderStatus::kUnknown:
      if (broker != nullptr) {
        return reconcile_unknown(handle, *broker);
      }
      // Handed to the adapter, and the broker has no record: it never arrived, or was dropped. Not re-sent.
      return oms_->apply(handle, OrderEvent::kReconcileRejected, {.reason_code = kReconMismatch}).has_value()
                 ? Recovery::kResolved
                 : Recovery::kUnresolved;
    case OrderStatus::kOpen:
    case OrderStatus::kPartiallyFilled:
    case OrderStatus::kCancelPending:
    case OrderStatus::kModifyPending:
      return broker != nullptr ? catch_up(handle, *broker) : unresolved(handle);
    case OrderStatus::kRiskRejected:
    case OrderStatus::kFilled:
    case OrderStatus::kCancelled:
    case OrderStatus::kBrokerRejected:
      return Recovery::kResolved;
  }
  return Recovery::kUnresolved;
}

}  // namespace vyuha::oms

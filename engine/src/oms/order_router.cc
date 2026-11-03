#include "vyuha/oms/order_router.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

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
  drain_refused();
  while (head_ < pending_.size()) {
    Pending& next = pending_[head_];
    if (!next.sent) {
      if (sink_->durable() < next.ticket || !sink_->healthy()) {
        break;
      }
      drain_refused();  // after durable(): a refused order's events count as durable, and it must not go out
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
    const auto recorded = oms_->record_sent(next.handle);
    if (!recorded.has_value() && recorded.error().code == ErrorCode::kCapacityExceeded) {
      break;  // the sink is full: try again on the next poll, and never send again
    }
    ++head_;  // anything else won't change by retrying, and mustn't hold up the orders behind it
  }
  if (head_ == pending_.size()) {
    pending_.clear();
    head_ = 0;
  }
  return sent;
}

void OrderRouter::drain_refused() noexcept {
  Uuid refused;
  while (sink_->try_pop_refused(refused)) {
    oms_->refuse(refused);
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): erasing from a reserved vector doesn't allocate
OrderRouter::CancelAll OrderRouter::cancel_all() noexcept {
  CancelAll out;
  pending_.erase(pending_.begin(),
                 pending_.begin() + static_cast<std::ptrdiff_t>(head_));  // already finished
  head_ = 0;
  std::erase_if(pending_, [](const Pending& p) { return !p.sent; });  // those never reach the broker
  for (OrderHandle handle = 0; handle < oms_->size(); ++handle) {
    const Order& order = oms_->order(handle);
    const OrderStatus status = order.status;
    // APPROVED means "maybe" (ADR-025): handed over with SENT not recorded yet, or restored and not
    // reconciled.
    const bool maybe_at_broker =
        status == OrderStatus::kApproved &&
        (order.needs_reconcile ||
         std::ranges::any_of(pending_, [handle](const Pending& p) { return p.sent && p.handle == handle; }));
    if (status == OrderStatus::kNew || (status == OrderStatus::kApproved && !maybe_at_broker) ||
        is_terminal(status)) {
      continue;
    }
    ++out.asked;
    if (!sender_->cancel(order)) {
      ++out.failed;
      continue;
    }
    if (status == OrderStatus::kOpen || status == OrderStatus::kPartiallyFilled) {
      // The broker has been asked either way. A full sink only leaves the status behind until the ack or the
      // next call: it never holds up the cancel itself.
      if (!oms_->apply(handle, OrderEvent::kCancelRequest).has_value()) {
        ++out.unrecorded;
      }
    }
  }
  return out;
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
  const Order& order = oms_->order(handle);
  const std::int32_t qty = order.details.qty;
  const std::int32_t filled = broker.filled_qty;
  // A book that can't be true of this order: a modify or fills we have no record of. Never guess (ADR-024).
  const bool contradicts = (broker.qty > 0 && broker.qty != qty) || filled < order.filled_qty ||
                           filled > qty || (broker.state == State::kOpen && filled >= qty) ||
                           (broker.state == State::kFilled && filled != qty) ||
                           (broker.state == State::kRejected && filled > 0);
  if (contradicts) {
    return unresolved(handle);
  }
  EventDetails details{.broker_order_id = broker.broker_order_id,
                       .fill_price = broker.average_price,
                       .broker_filled_qty = broker.filled_qty};
  OrderEvent event = OrderEvent::kReconcileOpen;
  switch (broker.state) {
    case State::kOpen:  // open with fills is what partly filled means
      event = filled > 0 ? OrderEvent::kReconcilePartial : OrderEvent::kReconcileOpen;
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
  const OrderStatus before = oms_->order(handle).status;
  const bool broker_gave_values =
      broker.qty > 0 || broker.price.has_value() || broker.trigger_price.has_value();
  if (before == OrderStatus::kModifyPending && broker_gave_values) {
    // The answer to the modify may have been lost in the crash: the book says how it ended.
    if (!oms_->apply(handle, OrderEvent::kModifyConfirm,
                     {.modified_qty = broker.qty,
                      .modified_price = broker.price,
                      .modified_trigger = broker.trigger_price})
             .has_value()) {
      return unresolved(handle);
    }
  } else if ((before == OrderStatus::kOpen || before == OrderStatus::kPartiallyFilled) && broker.qty > 0 &&
             broker.qty != oms_->order(handle).details.qty) {
    return unresolved(handle);  // a modify we have no record of: never guess
  }
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
  const auto looked_up = book.find_by_tag(broker_tag(order.details.client_order_id.view(), tag_limit_));
  if (!looked_up.has_value()) {
    // No answer isn't "not there": expiring on it would disown a live order. It stays blocked: try again.
    if (quill::Logger* logger = log::engine(); logger != nullptr) {
      LOG_ERROR(logger, "recovery: the broker's order book couldn't be read");
    }
    return Recovery::kUnresolved;
  }
  const std::vector<BrokerOrderView>& found = *looked_up;
  if (found.size() > 1) {
    return unresolved(handle);  // never guess which one is ours
  }
  return settle(handle, found.empty() ? nullptr : found.data());
}

OrderRouter::Recovery OrderRouter::settle(OrderHandle handle, const BrokerOrderView* broker) {
  switch (oms_->order(handle).status) {
    case OrderStatus::kNew:
      if (broker != nullptr) {  // it can't have been sent: APPROVED was never durable
        return unresolved(handle);
      }
      return oms_->apply(handle, OrderEvent::kRiskReject, {.reason_code = kNeverApproved}).has_value()
                 ? Recovery::kResolved
                 : Recovery::kUnresolved;
    case OrderStatus::kApproved:
      if (broker ==
          nullptr) {  // never reached the broker, and now it never will. Still blocked if unrecorded.
        return oms_->apply(handle, OrderEvent::kExpire, {.reason_code = kExpiredOnRestart}).has_value()
                   ? Recovery::kResolved
                   : Recovery::kUnresolved;
      }
      // needs_reconcile stays set until the reconcile below lands, so this can't look sendable if the sink is
      // full. The broker has it: a send would be a second copy.
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
    case OrderStatus::kExpired:
      return Recovery::kResolved;
  }
  return Recovery::kUnresolved;
}

}  // namespace vyuha::oms

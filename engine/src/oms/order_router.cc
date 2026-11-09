#include "vyuha/oms/order_router.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

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

namespace {

std::expected<Nanos, Error> parse_ms(std::string_view text, Nanos fallback, std::int64_t max_ms,
                                     std::string_view what) noexcept {
  if (text.empty()) {
    return fallback;
  }
  std::int64_t ms = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), ms);
  if (ec != std::errc{} || end != text.data() + text.size() || ms < 1 || ms > max_ms) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, what});
  }
  return ms * 1'000'000;
}

}  // namespace

std::expected<Nanos, Error> parse_approval_max_age_ms(std::string_view text) noexcept {
  return parse_ms(text, kDefaultApprovalMaxAge, kMaxApprovalMaxAgeMs,
                  "RISK_APPROVAL_MAX_AGE_MS: whole milliseconds, 1 to 10000");
}

std::expected<Nanos, Error> parse_queue_max_age_ms(std::string_view text) noexcept {
  return parse_ms(text, kDefaultQueueMaxAge, kMaxQueueMaxAgeMs,
                  "ORDER_QUEUE_MAX_AGE_MS: whole milliseconds, 1 to 60000");
}

OrderRouter::OrderRouter(Oms& oms, IOrderEventSink& sink, IOrderSender& sender, Venue venue,
                         const RouterConfig& config, const ISendLimits& limits)
    : oms_(&oms),
      sink_(&sink),
      sender_(&sender),
      venue_(venue),
      tag_limit_(config.tag_limit),
      capacity_(config.capacity),
      approval_max_age_(config.approval_max_age),
      queue_max_age_(config.queue_max_age),
      limits_(&limits),
      asked_(oms.capacity(), 0) {
  pending_.reserve(config.capacity);
  cancels_.reserve(oms.capacity());
}

// NOLINTNEXTLINE(bugprone-exception-escape): within the reserved capacity, so push_back can't allocate
std::expected<void, Error> OrderRouter::submit(OrderHandle handle, risk::ApprovedOrder approved) noexcept {
  if (pending_.size() >= capacity_) {
    return std::unexpected(Error{ErrorCode::kCapacityExceeded, "router: queue full"});
  }
  if (handle >= oms_->size() || oms_->order(handle).status != OrderStatus::kNew ||
      oms_->order(handle).needs_reconcile) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "router: order isn't NEW"});
  }
  if (approved.spent()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "router: the approval was moved from"});
  }
  if (!(approved.order() == oms_->order(handle).details)) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "router: the approval is for another order"});
  }
  if (approved.order().is_paper.paper() != (venue_ == Venue::kPaper)) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "router: paper and live don't mix"});
  }
  if (const Nanos age = oms_->clock().monotonic() - approved.stamp().monotonic;
      age < 0 || age > approval_max_age_) {
    const auto rejected =
        oms_->apply(handle, OrderEvent::kRiskReject,
                    {.reason_code = kApprovalStale, .gate = approved.stamp(), .approval_age = age});
    if (!rejected.has_value()) {
      return std::unexpected(rejected.error());  // the sink is full: the order stays NEW
    }
    ++stale_approvals_;
    return std::unexpected(Error{ErrorCode::kApprovalStale, "R-APPROVAL-STALE"});
  }
  const auto recorded = oms_->apply(handle, OrderEvent::kRiskApprove, {.gate = approved.stamp()});
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  pending_.push_back(Pending{.handle = handle, .ticket = sink_->pushed(), .approved = std::move(approved)});
  return {};
}

std::size_t OrderRouter::poll() noexcept {
  std::size_t sent = 0;
  drain_refused();
  const Nanos now = oms_->clock().monotonic();
  CancelAll ignored;
  drain_cancels(ignored);  // ahead of new orders: they take the tokens first (D-20)
  for (Pending& next : pending_) {
    if (!next.done && advance(next, now, sent) == Step::kStop) {
      break;
    }
  }
  std::erase_if(pending_, [](const Pending& p) { return p.done; });
  return sent;
}

// NOLINTNEXTLINE(bugprone-exception-escape): the tag fits small-string storage or the engine stops
OrderRouter::Step OrderRouter::advance(Pending& next, Nanos now, std::size_t& sent) noexcept {
  if (!next.sent) {
    if (sink_->durable() < next.ticket || !sink_->healthy()) {
      return Step::kStop;
    }
    drain_refused();  // after durable(): a refused order's events count as durable, and it must not go out
    if (!oms_->can_send(next.handle)) {  // cancelled or otherwise moved on while it waited
      next.done = true;
      return Step::kNext;
    }
    // ADR-027, on the monotonic clock (D-26). A negative age can't happen, and would expire: fail closed.
    const Nanos queued_for = now - next.approved.stamp().monotonic;
    if (queued_for > queue_max_age_ || queued_for < 0) {
      if (!oms_->apply(next.handle, OrderEvent::kExpire, {.reason_code = kExpiredInQueue}).has_value()) {
        return Step::kStop;  // the sink is full: the next poll tries again, and it is never sent
      }
      ++expired_in_queue_;
      next.done = true;
      return Step::kNext;
    }
    const OrderDetails& details = next.approved.order();
    if (!has_token(details, oms_->clock().monotonic(), limits_->ops_cap(details.account_id))) {
      return Step::kNext;  // waits for a token, and the queue age limit above still applies to it
    }
    // The tag is rebuilt here, not stored per order; a client id is short and this is once per order.
    const std::string tag = broker_tag(details.client_order_id.view(), tag_limit_);
    if (!sender_->send(next.approved, tag)) {
      return Step::kStop;
    }
    const Nanos handed_over = oms_->clock().monotonic();  // when it left, not when this poll began
    take_token(details, handed_over);
    approval_to_send_.record(handed_over - next.approved.stamp().monotonic);
    next.sent = true;
    ++sent;
  }
  const auto recorded = oms_->record_sent(next.handle);
  if (!recorded.has_value() && recorded.error().code == ErrorCode::kCapacityExceeded) {
    return Step::kStop;  // the sink is full: try again on the next poll, and never send again
  }
  next.done = true;  // anything else won't change by retrying, and mustn't hold up the orders behind it
  return Step::kNext;
}

OpsWindow& OrderRouter::bucket(const Uuid& account, std::uint8_t venue) noexcept {
  for (SendBucket& b : buckets_) {
    if (!b.used) {
      b = SendBucket{.account = account, .venue = venue, .used = true};
      return b.window;
    }
    if (b.account == account && b.venue == venue) {
      return b.window;
    }
  }
  return buckets_.back().window;  // more accounts than expected: they share a budget, which is only stricter
}

// An instrument not in the master could be on either exchange, so it needs a token from both and uses both.
bool OrderRouter::has_token(const OrderDetails& details, Nanos now, std::uint8_t cap) noexcept {
  const std::uint8_t venue = limits_->ops_venue(details.instrument_id);
  if (venue != kOpsVenueUnknown) {
    return bucket(details.account_id, venue).has_token(now, cap);
  }
  return bucket(details.account_id, 0).has_token(now, cap) &&
         bucket(details.account_id, 1).has_token(now, cap);
}

void OrderRouter::take_token(const OrderDetails& details, Nanos now) noexcept {
  const std::uint8_t venue = limits_->ops_venue(details.instrument_id);
  if (venue != kOpsVenueUnknown) {
    bucket(details.account_id, venue).take(now);
    return;
  }
  bucket(details.account_id, 0).take(now);
  bucket(details.account_id, 1).take(now);
}

// NOLINTNEXTLINE(bugprone-exception-escape): a failure here only logs static text
bool OrderRouter::drop_unsent(Pending& pending) noexcept {
  pending.done = true;
  const Order& order = oms_->order(pending.handle);
  if (order.status != OrderStatus::kApproved || order.needs_reconcile) {
    return false;  // storage refused it, or it has moved on: nothing to end
  }
  // A full sink leaves the order APPROVED until the restart rule expires it. Either way it never goes out.
  if (!oms_->apply(pending.handle, OrderEvent::kExpire, {.reason_code = kCancelledInQueue}).has_value()) {
    if (quill::Logger* logger = log::engine(); logger != nullptr) {
      LOG_ERROR(logger, "a cancelled order's expiry couldn't be recorded: the event sink is full");
    }
  }
  return true;
}

bool OrderRouter::may_be_at_broker(OrderHandle handle) const noexcept {
  const Order& order = oms_->order(handle);
  if (order.status == OrderStatus::kNew || is_terminal(order.status)) {
    return false;
  }
  // APPROVED means "maybe" (ADR-025): handed over with SENT not recorded yet, or restored and not reconciled.
  return order.status != OrderStatus::kApproved || order.needs_reconcile ||
         std::ranges::any_of(pending_, [handle](const Pending& p) { return p.sent && p.handle == handle; });
}

// NOLINTNEXTLINE(bugprone-exception-escape): resize only shrinks, so nothing here can throw
void OrderRouter::drain_cancels(CancelAll& out) noexcept {
  std::size_t kept = 0;
  for (const OrderHandle handle : cancels_) {
    const Order& order = oms_->order(handle);
    if (!may_be_at_broker(handle)) {
      continue;  // it finished while it waited
    }
    // A cancel must not be stopped by an account with no cap on file, so it falls back to the default.
    const std::uint8_t cap = limits_->ops_cap(order.details.account_id);
    if (!has_token(order.details, oms_->clock().monotonic(), cap == 0 ? kDefaultOpsCap : cap)) {
      cancels_[kept++] = handle;
      ++out.queued;
      continue;
    }
    ++out.asked;
    const OrderStatus status = order.status;
    if (!sender_->cancel(order)) {
      ++out.failed;
      cancels_[kept++] = handle;  // the adapter sent nothing, so no token is used: the next poll retries
      continue;
    }
    take_token(order.details, oms_->clock().monotonic());
    asked_[handle] = 1;
    if (status == OrderStatus::kOpen || status == OrderStatus::kPartiallyFilled) {
      // The broker has been asked either way. A full sink only leaves the status behind until the ack or the
      // next call: it never holds up the cancel itself.
      if (!oms_->apply(handle, OrderEvent::kCancelRequest).has_value()) {
        ++out.unrecorded;
      }
    }
  }
  cancels_.resize(kept);
}

void OrderRouter::drain_refused() noexcept {
  Uuid refused;
  while (sink_->try_pop_refused(refused)) {
    oms_->refuse(refused);
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): both vectors are reserved, so nothing here allocates
OrderRouter::CancelAll OrderRouter::cancel_all() noexcept {
  drain_refused();  // a refusal still in the sink would make a refused order look cancellable
  for (Pending& p : pending_) {
    if (!p.done && !p.sent) {
      drop_unsent(p);
    }
  }
  std::erase_if(pending_, [](const Pending& p) { return p.done; });
  cancels_.clear();  // built again: every order once, and the ones not asked yet at the front
  for (const std::uint8_t already_asked : {std::uint8_t{0}, std::uint8_t{1}}) {
    for (OrderHandle handle = 0; handle < oms_->size(); ++handle) {
      if (asked_[handle] == already_asked && may_be_at_broker(handle)) {
        cancels_.push_back(handle);
      }
    }
  }
  CancelAll out;
  drain_cancels(out);
  return out;
}

// NOLINTNEXTLINE(bugprone-exception-escape): reserved for every order, and each is queued at most once
bool OrderRouter::cancel(OrderHandle handle) noexcept {
  drain_refused();  // a refusal still in the sink would make a refused order look cancellable
  for (Pending& p : pending_) {
    if (p.handle == handle && !p.done && !p.sent) {
      return drop_unsent(p);
    }
  }
  if (handle >= oms_->size() || !may_be_at_broker(handle)) {
    return false;
  }
  if (std::ranges::find(cancels_, handle) == cancels_.end()) {
    cancels_.push_back(handle);
  }
  CancelAll ignored;
  drain_cancels(ignored);
  return true;
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

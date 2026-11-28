#include "vyuha/oms/slice_runner.h"

#include <algorithm>
#include <utility>

#include "vyuha/risk/rules.h"

namespace vyuha::oms {

namespace {

constexpr std::string_view kSliceDropped = "SLICE_DROPPED";

}  // namespace

SliceRunner::SliceRunner(const risk::RiskGate& gate, risk::GateContext& ctx, Oms& oms, OrderRouter& router,
                         IOrderEventSink& sink, IOrderIdSource& ids, const SliceRunnerConfig& config)
    : gate_(&gate),
      ctx_(&ctx),
      oms_(&oms),
      router_(&router),
      sink_(&sink),
      ids_(&ids),
      max_children_(std::min(config.max_children, FreezeSlicer::kMaxChildren)),
      parents_(config.max_parents),
      originals_(config.max_parents, OrderDetails{.is_paper = kPaperOrder}),
      children_(config.max_parents * max_children_, OrderDetails{.is_paper = kPaperOrder}),
      handles_(config.max_parents * max_children_, 0) {
  order_.reserve(config.max_parents);
}

SliceRunner::~SliceRunner() { router_->release(*this); }

bool SliceRunner::has_queued(std::size_t slot) noexcept {
  for (std::size_t i = 0; i < parents_[slot].next; ++i) {
    if (oms_->order(handle_of(slot, i)).status == OrderStatus::kApproved) {
      return true;
    }
  }
  return false;
}

// What the OMS shows now stops a parent before anything else does, so a failure that was already there is the
// reason, not the cancel that came after it.
void SliceRunner::notice(std::size_t slot) noexcept {
  if (!open(parents_[slot])) {
    return;
  }
  if (paused_since(parents_[slot].checked)) {
    stop(slot, SliceStop::kPaused);
  } else if (const std::optional<SliceStop> failed = child_failure(slot); failed.has_value()) {
    stop(slot, *failed);
  }
}

void SliceRunner::stop_unsent(std::size_t slot, SliceStop why) noexcept {
  if (sending(parents_[slot]) || has_queued(slot)) {
    stop(slot, why);
  }
}

ISliceHold::Answer SliceRunner::held_back(const OrderDetails& queued) noexcept {
  for (const std::size_t slot : order_) {
    if (child(slot, 0).slice != queued.slice) {
      continue;
    }
    const Parent& p = parents_[slot];
    if (open(p)) {
      if (paused_since(p.checked)) {
        stop(slot, SliceStop::kPaused);
      } else if (const std::optional<SliceStop> failed = child_failure(slot); failed.has_value()) {
        stop(slot, *failed);
      } else if (p.closing && !risk::RiskGate::reduces(*ctx_, queued)) {
        // D-55 at send time, so it doesn't depend on which of the router and the runner polls first.
        // M2.6: when the book counts our own working orders, leave this queued child's own working
        // quantity out here, as it is in the book already and must not count against itself.
        stop(slot, SliceStop::kNotReducing);
      }
    }
    switch (p.stopped) {
      case SliceStop::kNone:
        return Answer::kGo;
      case SliceStop::kCancelled:
      case SliceStop::kDropped:
        return Answer::kCancelled;
      default:
        return Answer::kStopped;
    }
  }
  return Answer::kStopped;  // no parent of it is held here, so nothing says it may still go
}

void SliceRunner::child_cancelled(const OrderDetails& queued) noexcept {
  for (const std::size_t slot : order_) {
    if (child(slot, 0).slice == queued.slice) {
      stop(slot, SliceStop::kChildExpired);  // as its recorded expiry would, so no sibling is gated meanwhile
      return;
    }
  }
}

void SliceRunner::watch(std::size_t slot) noexcept {
  Parent& p = parents_[slot];
  if (!open(p) || p.next < p.count) {
    return;  // stopped or done with; one still sending is advance()'s to watch
  }
  // Failures first: the last queued child may have just failed or expired.
  if (const std::optional<SliceStop> failed = child_failure(slot); failed.has_value()) {
    stop(slot, *failed);
  } else if (!has_queued(slot)) {
    p.settled = true;
  } else if (!sink_->healthy()) {
    stop(slot, SliceStop::kOrderLog);
  }
}

OrderDetails& SliceRunner::child(std::size_t slot, std::size_t i) noexcept {
  return children_[(slot * max_children_) + i];
}

OrderHandle& SliceRunner::handle_of(std::size_t slot, std::size_t i) noexcept {
  return handles_[(slot * max_children_) + i];
}

std::optional<std::size_t> SliceRunner::find(const Uuid& parent_id) const noexcept {
  for (const std::size_t slot : order_) {
    if (originals_[slot].id == parent_id) {
      return slot;
    }
  }
  return std::nullopt;
}

// Nothing left to send or write, and no child that could still need cancelling.
bool SliceRunner::evictable(std::size_t slot) noexcept {
  const Parent& p = parents_[slot];
  if (sending(p) || p.record != Record::kNone) {
    return false;
  }
  for (std::size_t i = 0; i < p.next; ++i) {
    if (!is_terminal(oms_->order(handle_of(slot, i)).status)) {
      return false;
    }
  }
  return true;
}

// An unused slot, or the oldest finished parent's. Nothing is changed: start() claims it.
std::optional<std::size_t> SliceRunner::free_slot() noexcept {
  for (std::size_t slot = 0; slot < parents_.size(); ++slot) {
    if (!parents_[slot].used) {
      return slot;
    }
  }
  for (const std::size_t slot : order_) {
    if (evictable(slot)) {
      return slot;
    }
  }
  return std::nullopt;
}

// As the router sees it (D-36): the wall clock moved more than kPauseGap past the monotonic one since
// `stamp`.
bool SliceRunner::paused_since(const risk::GateStamp& stamp) const noexcept {
  const IClock& clock = oms_->clock();
  return (clock.now() - stamp.ts) - (clock.monotonic() - stamp.monotonic) > kPauseGap;
}

// D-36: a slice is never resumed across a pause. PauseWatch compares two looks; the stamp check needs no
// earlier look.
void SliceRunner::observe_pause() noexcept {
  const bool paused = pause_.observe(oms_->clock());
  for (const std::size_t slot : order_) {
    if (open(parents_[slot]) && (paused || paused_since(parents_[slot].checked))) {
      stop_unsent(slot, SliceStop::kPaused);
    }
  }
}

// The one place that decides whether, and into what size, an order is sliced.
std::expected<std::int32_t, Error> SliceRunner::child_size(const OrderDetails& parent, SliceStart& answer,
                                                           bool& closing) noexcept {
  answer = SliceStart{.kind = SliceStart::Kind::kNotSliced};
  const risk::InstrumentRef* instrument =
      ctx_->instruments != nullptr ? ctx_->instruments->find(parent.instrument_id) : nullptr;
  // Only F&O is sliced, for now (D-51, D-52): an equity takes the normal path whatever its freeze value says,
  // a bad one included. Slicing equities is an M3 item, once the broker docs confirm it.
  if (instrument == nullptr ||
      (instrument->segment != risk::Segment::kFut && instrument->segment != risk::Segment::kOpt)) {
    return 0;
  }
  // At most one lot is never a parent, whatever the master data says: a trader can always exit a lot at a
  // time (D-54). The gate lets it through.
  if (parent.qty <= instrument->lot_size) {
    return 0;
  }
  const std::int32_t largest = FreezeSlicer::largest_child(instrument->freeze_qty, instrument->lot_size);
  if (largest != 0) {
    return parent.qty <= largest ? 0 : largest;
  }
  // No usable freeze quantity, so it can only go a lot at a time (D-51, D-54). A pure close is cut into
  // one-lot children (D-55).
  if (instrument->lot_size > 0 && risk::RiskGate::reduces(*ctx_, parent)) {
    closing = true;
    return instrument->lot_size;
  }
  if (instrument->freeze_qty <= 0) {
    // D-51: more than one lot with no freeze quantity is the gate's to refuse (R-FREEZE-UNKNOWN).
    const risk::GateCheck check = gate_->check(*ctx_, parent);
    if (!check.passed()) {
      answer = SliceStart{.kind = SliceStart::Kind::kRefused, .check = check};
    }
    return 0;
  }
  return std::unexpected(
      Error{ErrorCode::kInvalidArgument, "slicer: no whole lot below the freeze quantity"});
}

// NOLINTNEXTLINE(bugprone-exception-escape): push_back is within the reserved capacity
std::expected<SliceStart, Error> SliceRunner::start(const OrderDetails& parent) noexcept {
  observe_pause();
  if (parent.slice.sliced() || parent.slice_parent.has_value()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "slicer: the order is already a slice"});
  }
  const std::string_view client_id = parent.client_order_id.view();
  for (const std::size_t slot : order_) {
    if (originals_[slot].client_order_id.view() != client_id) {
      continue;
    }
    if (!same_request(originals_[slot], parent)) {
      return std::unexpected(Error{ErrorCode::kOrderDup, "ORDER_DUP"});
    }
    return SliceStart{.kind = SliceStart::Kind::kExisting};
  }
  if (oms_->find_handle(client_id).has_value()) {
    return SliceStart{.kind = SliceStart::Kind::kNotSliced};
  }
  SliceStart not_sliced;
  bool closing = false;
  const auto sized = child_size(parent, not_sliced, closing);
  if (!sized.has_value()) {
    return std::unexpected(sized.error());
  }
  const std::int32_t largest = *sized;
  if (largest == 0) {
    return not_sliced;
  }
  if (client_id.empty() || client_id.size() > FreezeSlicer::kMaxParentClientIdSize) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "slicer: client_order_id can't be suffixed"});
  }
  const std::size_t count = FreezeSlicer::child_count(parent.qty, largest);
  if (count > max_children_) {
    return std::unexpected(Error{ErrorCode::kCapacityExceeded, "slicer: too many children"});
  }
  FixedString<64> first;
  FreezeSlicer::suffix(first, client_id, 1);
  if (oms_->find_handle(first.view()).has_value() || find(parent.id).has_value()) {
    return std::unexpected(Error{ErrorCode::kOrderDup, "ORDER_DUP: parent already sliced"});
  }
  if (!sink_->healthy()) {
    sink_->note_outage_refusal(OutageRefusal::kNewOrder);
    return std::unexpected(
        Error{ErrorCode::kOrderLogUnavailable, "ORDER_LOG_UNAVAILABLE: new orders wait for the order log"});
  }
  const std::optional<std::size_t> slot = free_slot();
  if (!slot.has_value()) {
    return std::unexpected(Error{ErrorCode::kCapacityExceeded, "slicer: parent table full"});
  }

  SliceStart out{.kind = SliceStart::Kind::kRefused, .check = gate_->check(*ctx_, parent)};
  if (!out.check.passed()) {
    return out;
  }
  const std::size_t made = FreezeSlicer::slice(
      parent, largest, *ids_, std::span(children_).subspan(*slot * max_children_, max_children_));
  if (made != count) {  // every condition slice() checks was checked above
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "slicer: the order can't be sliced"});
  }
  std::erase(order_, *slot);  // if it held a finished parent
  order_.push_back(*slot);
  originals_[*slot] = parent;
  parents_[*slot] = Parent{
      .used = true, .requested = parent.qty, .count = made, .checked = out.check.stamp, .closing = closing};
  out.kind = SliceStart::Kind::kStarted;
  return out;
}

std::size_t SliceRunner::poll() noexcept {
  observe_pause();
  std::size_t sent = 0;
  for (const std::size_t slot : order_) {
    write_record(slot);
    while (sending(parents_[slot])) {
      if (advance(slot) != Step::kSent) {
        break;
      }
      ++sent;
    }
    watch(slot);
  }
  return sent;
}

// D-50: R-OPS refusing only because no token is free. A cap of 0 or an unknown account or instrument is a
// refusal like any other.
bool SliceRunner::waits_for_token(const risk::RuleResult& rejection,
                                  const OrderDetails& order) const noexcept {
  return rejection.code == risk::RuleCode::kOps && ctx_->instruments != nullptr &&
         ctx_->instruments->find(order.instrument_id) != nullptr &&
         std::as_const(*ctx_).accounts.find(order.account_id) != nullptr &&
         risk::effective_ops_cap(*ctx_, order.account_id) >= 1;
}

std::optional<SliceStop> SliceRunner::child_failure(std::size_t slot) noexcept {
  const Parent& p = parents_[slot];
  // Only the order log's refusal (Oms::refuse) can end a child still waiting to be submitted, or leave a
  // submitted one RISK_REJECTED.
  if (p.taken_in && oms_->order(p.handle).status != OrderStatus::kNew) {
    return SliceStop::kStorageRefused;
  }
  for (std::size_t i = 0; i < p.next; ++i) {
    const Order& sent = oms_->order(handle_of(slot, i));
    if (sent.status == OrderStatus::kRiskRejected) {
      return SliceStop::kStorageRefused;
    }
    if (sent.status == OrderStatus::kBrokerRejected) {
      return SliceStop::kChildBrokerRejected;
    }
    if (sent.status == OrderStatus::kExpired) {
      return SliceStop::kChildExpired;
    }
    if (sent.status == OrderStatus::kUnknown || sent.went_unknown) {
      return SliceStop::kChildUnknown;
    }
  }
  return std::nullopt;
}

SliceRunner::Step SliceRunner::advance(std::size_t slot) noexcept {
  Parent& p = parents_[slot];
  // Again before every child: a pause can fall inside a poll (D-36).
  if (paused_since(p.checked)) {
    stop(slot, SliceStop::kPaused);
    return Step::kDone;
  }
  if (const std::optional<SliceStop> failed = child_failure(slot); failed.has_value()) {
    stop(slot, *failed);
    return Step::kDone;
  }
  if (!sink_->healthy()) {
    sink_->note_outage_refusal(OutageRefusal::kNewOrder);
    stop(slot, SliceStop::kOrderLog);
    return Step::kDone;
  }
  const OrderDetails& order = child(slot, p.next);
  // D-55: before anything is waited for or used up. Someone else may have closed the position.
  if (p.closing && !risk::RiskGate::reduces(*ctx_, order)) {
    stop(slot, SliceStop::kNotReducing);
    return Step::kDone;
  }
  // Checked before the gate, so that nothing is used up for a child that can't be taken further.
  if (router_->pending() >= router_->capacity()) {
    return Step::kWait;
  }
  if (!p.taken_in) {
    if (oms_->find_handle(order.client_order_id.view()).has_value()) {
      stop(slot, SliceStop::kDuplicate);  // someone else's order: never submitted from here
      return Step::kDone;
    }
    if (oms_->size() >= oms_->capacity()) {
      stop(slot, SliceStop::kPoolFull);
      return Step::kDone;
    }
  }

  risk::GateDecision decision = gate_->evaluate(*ctx_, order);
  if (!decision.approved.has_value()) {
    const risk::RuleResult rejection = decision.rejection.value_or(risk::RuleResult{});
    if (waits_for_token(rejection, order)) {
      return Step::kWait;
    }
    // Latched before it is written: a refused child is never gated again, whatever happens to the record.
    p.stopped = SliceStop::kRefused;
    p.rejection = rejection;
    p.refused = decision.stamp;
    p.record = Record::kRefusal;
    write_record(slot);
    return Step::kDone;
  }

  return hand_over(slot, std::move(*decision.approved));
}

// Takes the approved child into the OMS, unless an earlier try did, and submits it. Only a successful submit
// queues it, and that moves the parent on at once.
SliceRunner::Step SliceRunner::hand_over(std::size_t slot, risk::ApprovedOrder approved) noexcept {
  Parent& p = parents_[slot];
  const OrderDetails& order = child(slot, p.next);
  if (!p.taken_in) {
    const auto taken = oms_->intake(order);
    if (!taken.has_value()) {
      switch (taken.error().code) {
        case ErrorCode::kCapacityExceeded:  // the sink is full (the pool was checked): nothing was recorded.
          return Step::kWait;  // The retry gates again and uses an approval up again: see the class comment.
        case ErrorCode::kOrderLogUnavailable:
          stop(slot, SliceStop::kOrderLog);
          return Step::kDone;
        case ErrorCode::kOrderDup:
          stop(slot, SliceStop::kDuplicate);
          return Step::kDone;
        default:
          stop(slot, SliceStop::kError);
          return Step::kDone;
      }
    }
    if (taken->existing) {
      stop(slot, SliceStop::kDuplicate);
      return Step::kDone;
    }
    p.taken_in = true;
    p.handle = taken->handle;
  }

  const auto submitted = router_->submit(p.handle, std::move(approved), this);
  if (!submitted.has_value()) {
    switch (submitted.error().code) {
      case ErrorCode::kCapacityExceeded:  // not queued, and the order is still NEW
        return Step::kWait;
      case ErrorCode::kApprovalStale:  // the router recorded the rejection itself
        p.taken_in = false;
        stop(slot, SliceStop::kStaleApproval);
        return Step::kDone;
      default:
        stop(slot, SliceStop::kError);
        return Step::kDone;
    }
  }
  handle_of(slot, p.next) = p.handle;
  p.taken_in = false;
  p.sent_qty += order.qty;
  ++p.next;
  return Step::kSent;
}

void SliceRunner::stop(std::size_t slot, SliceStop why) noexcept {
  Parent& p = parents_[slot];
  if (!open(p)) {
    return;  // the first reason stands, or there is nothing left to stop
  }
  p.stopped = why;
  if (p.taken_in) {
    p.record = Record::kDropped;
    write_record(slot);
  }
}

void SliceRunner::give_up_record(Parent& p) noexcept {
  p.record = Record::kNone;
  ++unrecorded_;
}

// Writes what a stopped parent still owes the order log. A full sink or an outage keeps it for a later poll.
void SliceRunner::write_record(std::size_t slot) noexcept {
  Parent& p = parents_[slot];
  if (p.record == Record::kNone) {
    return;
  }
  if (!p.taken_in) {
    const OrderDetails& order = child(slot, p.next);
    if (oms_->find_handle(order.client_order_id.view()).has_value() || oms_->size() >= oms_->capacity()) {
      give_up_record(p);
      return;
    }
    if (!sink_->healthy()) {
      return;  // wait, without asking intake: it would count this as a refused order each time
    }
    const auto taken = oms_->intake(order);
    if (!taken.has_value()) {
      if (taken.error().code != ErrorCode::kCapacityExceeded &&
          taken.error().code != ErrorCode::kOrderLogUnavailable) {
        give_up_record(p);
      }
      return;
    }
    p.taken_in = true;
    p.handle = taken->handle;
  }
  if (oms_->order(p.handle).status != OrderStatus::kNew) {  // the order log refused it: it has ended
    p.record = Record::kNone;
    return;
  }
  const bool refusal = p.record == Record::kRefusal;
  const std::string_view reason =
      refusal ? risk::reason_text(p.rejection.value_or(risk::RuleResult{})) : kSliceDropped;
  EventDetails details{.reason_code = reason};
  if (refusal) {
    details.gate = p.refused;
  }
  const auto recorded = oms_->apply(p.handle, OrderEvent::kRiskReject, details);
  if (!recorded.has_value() && recorded.error().code == ErrorCode::kCapacityExceeded) {
    return;
  }
  if (!recorded.has_value()) {
    give_up_record(p);
    return;
  }
  p.record = Record::kNone;
}

bool SliceRunner::cancel(const Uuid& parent_id) noexcept {
  const std::optional<std::size_t> slot = find(parent_id);
  if (!slot.has_value()) {
    return false;
  }
  // Stopped first, so the cancels below pull back the queued children (D-56).
  notice(*slot);
  stop_unsent(*slot, SliceStop::kCancelled);
  for (std::size_t i = 0; i < parents_[*slot].next; ++i) {
    static_cast<void>(router_->cancel(handle_of(*slot, i)));  // false: that child has nothing left to cancel
  }
  return true;
}

void SliceRunner::drop_all() noexcept {
  for (const std::size_t slot : order_) {
    notice(slot);
    stop_unsent(slot, SliceStop::kDropped);
  }
}

std::optional<SliceView> SliceRunner::view(const Uuid& parent_id) const noexcept {
  const std::optional<std::size_t> slot = find(parent_id);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  const Parent& p = parents_[*slot];
  return SliceView{.parent_id = parent_id,
                   .requested = p.requested,
                   .children = p.count,
                   .children_sent = p.next,
                   .unsent_qty = p.requested - p.sent_qty,
                   .stopped = p.stopped,
                   .rejection = p.rejection};
}

}  // namespace vyuha::oms

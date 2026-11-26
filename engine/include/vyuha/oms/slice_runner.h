#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

#include "vyuha/core/error.h"
#include "vyuha/core/pause_watch.h"
#include "vyuha/oms/freeze_slicer.h"
#include "vyuha/oms/oms.h"
#include "vyuha/oms/order_router.h"
#include "vyuha/risk/gate_context.h"
#include "vyuha/risk/risk_gate.h"

namespace vyuha::oms {

// Why a parent's unsent remainder was dropped. kNone: nothing was dropped.
enum class SliceStop : std::uint8_t {
  kNone,
  kRefused,         // the gate refused a child (D-45): recorded RISK_REJECTED; SliceView::rejection says why
  kStorageRefused,  // the order log refused a child already submitted (it never goes out)
  kOrderLog,        // the order log is unhealthy (ADR-027)
  kPoolFull,        // the OMS holds no more orders
  kDuplicate,       // a child's client_order_id or id is already in use
  kStaleApproval,   // R-APPROVAL-STALE
  kError,           // intake or submit refused a child for any other reason
  kPaused,          // the engine was paused (D-36)
  kCancelled,       // cancel()
  kDropped,         // drop_all()
  // D-53: a child already sent failed after the gate.
  kChildBrokerRejected,  // BROKER_REJECTED
  kChildExpired,         // EXPIRED: it waited too long in the send queue, or was dropped from it
  kChildUnknown,         // UNKNOWN, even if it has been reconciled since
};

struct SliceView {
  Uuid parent_id{};
  std::int32_t requested{0};     // the parent's quantity
  std::size_t children{0};       // how many it was cut into
  std::size_t children_sent{0};  // approved and handed to the router's queue
  std::int32_t unsent_qty{0};    // requested less what those carry; dropped for good once stopped
  SliceStop stopped{SliceStop::kNone};
  std::optional<risk::RuleResult> rejection{};  // NOLINT(readability-redundant-member-init): with kRefused
};

struct SliceStart {
  enum class Kind : std::uint8_t {
    kStarted,    // held: poll() sends the children
    kExisting,   // this request is already held: nothing new was started
    kNotSliced,  // nothing was done: the caller takes the normal path (intake, gate, submit)
    kRefused,    // the gate refused the full size (D-49): nothing is held, recorded or sent
  };
  Kind kind{Kind::kNotSliced};
  risk::GateCheck check{};  // NOLINT(readability-redundant-member-init): with kStarted and kRefused
};

struct SliceRunnerConfig {
  std::size_t max_parents{64};
  std::size_t max_children{100};  // per parent; never above FreezeSlicer::kMaxChildren
};

// Sends an order too large for the exchange's freeze quantity as children, each through the gate (F-EX-3,
// F-RK-1, D-45, D-49, D-50).
//
// Only F&O orders (a future or an option) are sliced (D-51, D-52). An equity is never sliced and never
// refused for a freeze value, whatever the instrument master says, until equity slicing is confirmed against
// the broker docs (M3). An F&O parent is sliced when the quantity is above the largest child,
// FreezeSlicer::largest_child: the largest whole number of lots strictly below the freeze quantity (D-52).
// Core thread only. Everything is allocated at construction, and start, poll, cancel and drop_all allocate
// nothing.
//
// Nothing here is persisted. Each child sent is an order with a link to its parent (D-48), but the unsent
// remainder lives only in memory: after a restart it is gone and is never sent, which is the safe direction.
// By the same token a caller's idempotency beyond the parents held here is Postgres's order_slice_parent.
//
// What the writer protects behind that, and what it does not (M2.8 must give every parent a fresh id AND
// check Postgres for the parent's client_order_id before start):
//  - a client_order_id stored before, under a new parent id: every child is refused (the parent row's unique
//    client_order_id).
//  - a parent id reused with a different client_order_id, qty, account, instrument, side, order type,
//  product,
//    price, trigger or paper flag: every child is refused (the stored row is compared).
//  - a parent id reused with the SAME client_order_id and those fields, after a restart: the parent row
//    matches, so only the children's own unique client_order_ids catch it, one by one. A child numbered past
//    those stored the first time is accepted and SENT.
//
// A child is gated, then taken into the OMS, then submitted, in one pass with nothing in between. Only
// OrderRouter::submit queues an order for the adapter, and every error it returns comes before it queues, so:
//  - submit succeeded: the parent moves to the next child at once; that child is never gated or taken in
//  again.
//  - kept and tried again on a later poll, with nothing recorded or queued: no OPS token (R-OPS alone, D-50),
//    the router's queue full (not even gated), or a full event sink at intake or submit. For the last, the
//    gate runs again on the retry; a child already taken in (NEW) keeps its handle and is not taken in twice,
//    and a NEW order is in no send queue. What the first approval used up (an OPS token, reserved margin) is
//    not given back, which is only stricter.
//    (This includes a stale approval whose rejection the router couldn't record for the same full sink: it
//    returns kCapacityExceeded, and the retry is a new decision.)
//  - everything else drops the unsent remainder for good (SliceStop). Children already sent are never
//    cancelled here except by cancel().
//
// A full sink cannot be asked about before gating (IOrderEventSink has no such query), so each retry uses an
// approval up again: an OPS token, a duplicate-table entry and, for a live order, R-MARGIN's reservation,
// which its on_approved takes each time until the broker's next margin figure replaces it. The OPS cap bounds
// the retries to `cap` a second, and all of it only makes the gate stricter; a long outage of the sink can
// end with R-MARGIN refusing the child. A reservation that has to be exact must not be taken twice on this
// path.
//
// D-53: before each child is gated, every child already sent is looked at. If one is BROKER_REJECTED,
// EXPIRED, has been UNKNOWN, or was refused by the order log, the remainder is dropped for good. FILLED,
// PARTIALLY_FILLED and CANCELLED (an IOC's remainder, say) don't stop it. Children the router already holds
// are not touched: with a cap of N, up to N can be queued or at the broker before the first answer arrives.
//
// A child taken into the OMS (NEW) whose parent is then dropped before it could be submitted is ended
// RISK_REJECTED with reason SLICE_DROPPED: it never reached the router or the broker, and no rule refused it.
class SliceRunner {
 public:
  // Everything is borrowed and must outlive the runner. `gate` and `oms` must share one clock, and `sink`
  // must be the one `oms` and `router` use.
  SliceRunner(const risk::RiskGate& gate, risk::GateContext& ctx, Oms& oms, OrderRouter& router,
              IOrderEventSink& sink, IOrderIdSource& ids, const SliceRunnerConfig& config = {});

  // Idempotent on parent.client_order_id among the parents held: the same request (same_request) returns
  // kExisting, a different one is kOrderDup.
  //
  // An F&O instrument with no freeze quantity (0 or less) is never sliced: more than one lot of it goes to
  // the parent check, which refuses it (kRefused, R-FREEZE-UNKNOWN, D-51).
  //
  // kNotSliced when there is nothing to slice: the instrument is unknown or isn't F&O, the order is at most
  // one lot (whatever the freeze value, bad or unknown included, so a trader can always exit, D-54), the
  // quantity fits one child, or the OMS already holds an order
  // under this client_order_id (its intake decides whether that is a retry or a duplicate). Nothing is
  // recorded: the caller runs its normal intake, gate and submit.
  //
  // Errors, with nothing recorded or sent: kInvalidArgument (more than one lot and no whole lot below the
  // freeze quantity, which is bad master data; a parent already marked as a slice; a client_order_id too long
  // to suffix), kOrderDup (this parent was sliced before, or its id is taken), kCapacityExceeded (more than
  // max_children, or no room for another parent), kOrderLogUnavailable (ADR-027).
  //
  // A parent stays held, for idempotency and cancel, until its slot is needed: only one with nothing left to
  // send and no child still working is ever given up, the oldest first.
  [[nodiscard]] std::expected<SliceStart, Error> start(const OrderDetails& parent) noexcept;

  // For each parent in start order: gate the next child and, when approved, take it in and submit it, until
  // one has to wait or the parent is done or stopped. Returns how many children were submitted. The caller
  // polls the router separately.
  std::size_t poll() noexcept;

  // Drops the unsent remainder and asks the router to cancel every child still working. False if no parent
  // with this id is held. Safe to repeat.
  bool cancel(const Uuid& parent_id) noexcept;
  // Drops every unsent remainder, and cancels nothing: for the kill switch and shutdown, with
  // OrderRouter::cancel_all (M2.7).
  void drop_all() noexcept;

  [[nodiscard]] std::optional<SliceView> view(const Uuid& parent_id) const noexcept;
  // Children whose refusal (D-45) or dropped NEW order could never be recorded.
  [[nodiscard]] std::uint64_t unrecorded() const noexcept { return unrecorded_; }

 private:
  // What is still to be written for the child at `next` of a stopped parent.
  enum class Record : std::uint8_t {
    kNone,
    kRefusal,  // the gate refused it: NEW, then RISK_REJECTED (D-45)
    kDropped,  // taken in but never submitted: end the NEW order
  };
  enum class Step : std::uint8_t { kSent, kWait, kDone };

  struct Parent {
    bool used{false};
    std::int32_t requested{0};
    std::int32_t sent_qty{0};
    std::size_t count{0};  // children
    std::size_t next{0};   // the next child to send; also how many were sent
    SliceStop stopped{SliceStop::kNone};
    std::optional<risk::RuleResult> rejection{};  // NOLINT(readability-redundant-member-init)
    risk::GateStamp checked{};                    // the parent check: a pause since then drops the remainder
    risk::GateStamp refused{};                    // the refused child's
    Record record{Record::kNone};
    bool taken_in{false};  // the child at `next` is in the OMS (NEW) under `handle`
    OrderHandle handle{0};
  };

  [[nodiscard]] static bool sending(const Parent& p) noexcept {
    return p.stopped == SliceStop::kNone && p.next < p.count;
  }
  [[nodiscard]] OrderDetails& child(std::size_t slot, std::size_t i) noexcept;
  [[nodiscard]] OrderHandle& handle_of(std::size_t slot, std::size_t i) noexcept;
  [[nodiscard]] std::optional<std::size_t> find(const Uuid& parent_id) const noexcept;
  [[nodiscard]] bool evictable(std::size_t slot) noexcept;
  [[nodiscard]] std::optional<std::size_t> free_slot() noexcept;
  void observe_pause() noexcept;
  [[nodiscard]] bool paused_since(const risk::GateStamp& stamp) const noexcept;
  // The largest child to cut `parent` into, or 0 when it isn't to be sliced, with `answer` set to what
  // start() then returns.
  [[nodiscard]] std::expected<std::int32_t, Error> child_size(const OrderDetails& parent,
                                                              SliceStart& answer) noexcept;
  // Why a child already handed on, or taken in, ends the parent (D-53, and the order log's refusal).
  [[nodiscard]] std::optional<SliceStop> child_failure(std::size_t slot) noexcept;
  [[nodiscard]] Step advance(std::size_t slot) noexcept;
  [[nodiscard]] Step hand_over(std::size_t slot, risk::ApprovedOrder approved) noexcept;
  [[nodiscard]] bool waits_for_token(const risk::RuleResult& rejection,
                                     const OrderDetails& order) const noexcept;
  void stop(std::size_t slot, SliceStop why) noexcept;
  void write_record(std::size_t slot) noexcept;
  void give_up_record(Parent& p) noexcept;

  const risk::RiskGate* gate_;
  risk::GateContext* ctx_;
  Oms* oms_;
  OrderRouter* router_;
  IOrderEventSink* sink_;
  IOrderIdSource* ids_;
  std::size_t max_children_;
  std::vector<Parent> parents_;
  std::vector<OrderDetails> originals_;  // per slot: the parent as asked for
  std::vector<OrderDetails> children_;   // max_children_ per slot
  std::vector<OrderHandle> handles_;     // likewise: the handle of each child sent
  std::vector<std::size_t> order_;       // used slots, oldest first; reserved
  PauseWatch pause_;
  std::uint64_t unrecorded_{0};
};

}  // namespace vyuha::oms

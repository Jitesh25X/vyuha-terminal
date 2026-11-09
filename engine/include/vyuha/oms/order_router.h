#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vyuha/core/error.h"
#include "vyuha/core/latency_histogram.h"
#include "vyuha/core/pause_watch.h"
#include "vyuha/core/price.h"
#include "vyuha/oms/oms.h"
#include "vyuha/oms/ops_window.h"
#include "vyuha/risk/approved_order.h"

namespace vyuha::oms {

// The send side of a broker adapter. send takes only the gate's ApprovedOrder (invariant 1), so nothing can
// hand an adapter an order the gate hasn't passed. It must not block; false means "try again later", with
// nothing sent. broker_tag goes in the broker's tag field.
class IOrderSender {
 public:
  IOrderSender() = default;
  IOrderSender(const IOrderSender&) = delete;
  IOrderSender& operator=(const IOrderSender&) = delete;
  IOrderSender(IOrderSender&&) = delete;
  IOrderSender& operator=(IOrderSender&&) = delete;
  virtual ~IOrderSender() = default;
  // A spent approval approves nothing: an implementation returns false for it and sends nothing.
  [[nodiscard]] virtual bool send(const risk::ApprovedOrder& order, std::string_view broker_tag) noexcept = 0;
  // Asks the broker to cancel. A cancel only reduces risk, so it takes the order as the OMS has it. Repeating
  // it is harmless. The order may have no broker_order_id yet (handed over with no ack, or restored and not
  // reconciled): find it by broker_tag, or return false, never true for nothing.
  [[nodiscard]] virtual bool cancel(const Order& order) noexcept = 0;
};

struct BrokerOrderView {
  enum class State : std::uint8_t { kOpen, kPartiallyFilled, kFilled, kCancelled, kRejected };
  State state{State::kOpen};
  std::int32_t filled_qty{0};
  Price average_price{};          // NOLINT(readability-redundant-member-init): partly filled by designators
  std::string broker_order_id{};  // NOLINT(readability-redundant-member-init)
  // What the broker says the order now is. 0 or empty: the adapter didn't say. A modify changes these.
  std::int32_t qty{0};
  std::optional<Price> price{};          // NOLINT(readability-redundant-member-init)
  std::optional<Price> trigger_price{};  // NOLINT(readability-redundant-member-init)
};

// The broker's order book, as reconciliation reads it. Not used on the core thread's hot path.
class IBrokerOrderBook {
 public:
  IBrokerOrderBook() = default;
  IBrokerOrderBook(const IBrokerOrderBook&) = delete;
  IBrokerOrderBook& operator=(const IBrokerOrderBook&) = delete;
  IBrokerOrderBook(IBrokerOrderBook&&) = delete;
  IBrokerOrderBook& operator=(IBrokerOrderBook&&) = delete;
  virtual ~IBrokerOrderBook() = default;
  // Every order the broker has with this tag. An error means the book couldn't be read, which isn't "none":
  // an adapter must never report a failed read as an empty list, because recovery expires on empty.
  [[nodiscard]] virtual std::expected<std::vector<BrokerOrderView>, Error> find_by_tag(
      std::string_view broker_tag) = 0;
};

// client_order_id in the broker's tag field (ADR-025): its letters and digits, keeping the last `limit`. The
// tail, because the start of a UUIDv7 is a timestamp shared by orders made together.
[[nodiscard]] std::string broker_tag(std::string_view client_order_id, std::size_t limit);

// Which orders a router's sender takes. A paper router refuses live approvals and a live one refuses paper
// ones, so a flag left at its paper default can't reach a broker with only the paper rules behind it.
enum class Venue : std::uint8_t { kPaper, kLive };

// An approval is evaluated and submitted in the same pass of the core thread. Older than the limit, the kill
// switch or the limits may have changed since. Owner, 2026-11-08: 100 ms by default, set by
// RISK_APPROVAL_MAX_AGE_MS.
inline constexpr Nanos kDefaultApprovalMaxAge = kNanosPerSecond / 10;  // 100 ms
inline constexpr std::int64_t kMaxApprovalMaxAgeMs = 10'000;           // a sanity ceiling on the setting

// A queued order (APPROVED, not yet sent) older than this, from its gate stamp, is never sent: it becomes
// EXPIRED (EXPIRED_IN_QUEUE), audited and alerted (ADR-027). Owner, 2026-11-07: 5 s by default, set by
// ORDER_QUEUE_MAX_AGE_MS. It bounds how stale an order can be when an outage ends.
inline constexpr Nanos kDefaultQueueMaxAge = 5 * kNanosPerSecond;
inline constexpr std::int64_t kMaxQueueMaxAgeMs = 60'000;

// RISK_APPROVAL_MAX_AGE_MS: whole milliseconds, 1 to 10,000. Unset or empty is the default; anything else is
// kInvalidArgument, and the engine must refuse to start rather than guess.
[[nodiscard]] std::expected<Nanos, Error> parse_approval_max_age_ms(std::string_view text) noexcept;
// ORDER_QUEUE_MAX_AGE_MS: the same rules, 1 to 60,000.
[[nodiscard]] std::expected<Nanos, Error> parse_queue_max_age_ms(std::string_view text) noexcept;

// What the send queue needs to pace orders (D-20). Core thread.
class ISendLimits {
 public:
  ISendLimits() = default;
  ISendLimits(const ISendLimits&) = delete;
  ISendLimits& operator=(const ISendLimits&) = delete;
  ISendLimits(ISendLimits&&) = delete;
  ISendLimits& operator=(ISendLimits&&) = delete;
  virtual ~ISendLimits() = default;
  // Orders per second for this account on one exchange, at most kMaxOpsCap. 0: no new order may go.
  [[nodiscard]] virtual std::uint8_t ops_cap(const Uuid& account) const noexcept = 0;
  // Which exchange's budget the instrument uses: below kOpsVenues, or kOpsVenueUnknown.
  [[nodiscard]] virtual std::uint8_t ops_venue(std::int64_t instrument_id) const noexcept = 0;
};

struct RouterConfig {
  std::size_t tag_limit{0};
  std::size_t capacity{0};
  Nanos approval_max_age{kDefaultApprovalMaxAge};
  Nanos queue_max_age{kDefaultQueueMaxAge};
};

// Persist before send (ADR-025). The core thread submits an approved order and polls; nothing here touches
// Postgres. An order goes to the adapter only once its APPROVED event is durable, and is recorded SENT after
// the adapter has it, so in Postgres SENT means "handed over" and APPROVED means "maybe".
class OrderRouter {
 public:
  enum class Recovery : std::uint8_t {
    kResolved,    // matches the broker now
    kUnresolved,  // a mismatch is raised or the sink is full, and the order stays blocked until retried
  };

  // The venue is a separate argument so no default can choose it. limits is borrowed and must outlive this.
  OrderRouter(Oms& oms, IOrderEventSink& sink, IOrderSender& sender, Venue venue, const RouterConfig& config,
              const ISendLimits& limits);
  OrderRouter(Oms& oms, IOrderEventSink& sink, IOrderSender& sender, Venue venue, const RouterConfig& config,
              const ISendLimits&& limits) = delete;

  // Records APPROVED, with the gate's stamp, and queues the order to send what the gate approved. The order
  // must be NEW, and the approval must be for exactly its details, for this router's venue and unspent:
  // anything else is kInvalidArgument and changes nothing.
  //
  // An approval older than approval_max_age by the OMS's clock, or dated in its future, ends the order: it is
  // recorded RISK_REJECTED with R-APPROVAL-STALE and the stamp, which the writer audits and alerts in the
  // same transaction, and the result is kApprovalStale. Nothing retries it: a new order is a new decision.
  //
  // A full sink is kCapacityExceeded and the order stays NEW. Only this queue reaches the adapter, so an
  // order marked APPROVED some other way (Oms::apply, recovery) is never sent.
  [[nodiscard]] std::expected<void, Error> submit(OrderHandle handle, risk::ApprovedOrder approved) noexcept;
  // Sends queued cancels first, then what's durable, in order; returns how many new orders it handed to the
  // adapter. Never blocks. Everything sent uses the account's per-exchange budget, so at most its cap go in
  // any rolling second (D-20). An order with no token waits, and orders of other accounts or exchanges go
  // past it. An order whose turn comes after queue_max_age from its gate stamp is expired instead; if that
  // can't be recorded (a full sink) the queue waits, and it is never sent.
  std::size_t poll() noexcept;
  [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }
  [[nodiscard]] std::size_t cancels_queued() const noexcept { return cancels_.size(); }
  // Gate stamp to handing over to the adapter, per order sent: vyuha_engine_approval_to_send_seconds. It
  // includes waiting for the APPROVED event to be durable, so it shows how close real latency gets to
  // the approval limit.
  [[nodiscard]] const LatencyHistogram& approval_to_send() const noexcept { return approval_to_send_; }
  [[nodiscard]] std::uint64_t stale_approvals() const noexcept { return stale_approvals_; }
  [[nodiscard]] std::uint64_t expired_in_queue() const noexcept { return expired_in_queue_; }
  [[nodiscard]] std::uint64_t expired_after_pause() const noexcept { return expired_after_pause_; }

  // Cancels everything the broker may hold, for the kill switch (M2.7). Nothing here waits on or is refused
  // by the order log: a cancel only reduces risk, so an outage, a full ring or a refused order can't stop
  // one. Unsent orders leave the queue. An APPROVED order is asked too if it may be at the broker.
  //
  // Cancels go ahead of new orders but count toward the per-second limit, so a kill burst stays compliant
  // (D-20): what the budget allows goes now, the rest waits in `queued` and poll sends it. false from the
  // adapter is counted in `failed` and poll retries it. Calling again is harmless: orders not yet asked go
  // first, so repeats can't starve them. An unsent order leaves the queue as EXPIRED (CANCELLED_IN_QUEUE).
  struct CancelAll {
    std::size_t asked{0};
    std::size_t failed{0};
    std::size_t unrecorded{
        0};                 // asked, but the ring was full: the status catches up on the ack or the next call
    std::size_t queued{0};  // waiting for a token
  };
  CancelAll cancel_all() noexcept;
  // One order's cancel, under the same rules. An order still waiting to be sent is dropped and never goes
  // out. False if there is nothing to cancel.
  bool cancel(OrderHandle handle) noexcept;

  // After a restart, for each restored order: find it at the broker by tag and settle it. Never sends or
  // queues anything (the owner's restart rule, ADR-025): an order the broker has is adopted, and an APPROVED
  // order it lacks becomes EXPIRED, because a stale order surfacing after a crash must never fire.
  [[nodiscard]] Recovery recover(OrderHandle handle, IBrokerOrderBook& book);

 private:
  struct Pending {
    OrderHandle handle{0};
    std::uint64_t ticket{0};         // durable() must reach this
    bool sent{false};                // the adapter has it; only recording is left
    bool done{false};                // nothing left to do: poll removes it
    bool expire_after_pause{false};  // a pause was seen while it waited (D-36): expire it, never send it
    risk::ApprovedOrder approved;
  };
  struct SendBucket {
    Uuid account{};
    std::uint8_t venue{0};
    bool used{false};
    OpsWindow window{};
  };
  static constexpr std::size_t kSendBuckets = 128;

  [[nodiscard]] Recovery reconcile_unknown(OrderHandle handle, const BrokerOrderView& broker);
  [[nodiscard]] Recovery catch_up(OrderHandle handle, const BrokerOrderView& broker);
  [[nodiscard]] Recovery settle(OrderHandle handle,
                                const BrokerOrderView* broker);  // nullptr: not at the broker
  [[nodiscard]] Recovery unresolved(OrderHandle handle);
  void drain_refused() noexcept;
  enum class Step : std::uint8_t { kNext, kStop };
  [[nodiscard]] Step advance(Pending& next, Nanos now, std::size_t& sent) noexcept;
  [[nodiscard]] bool paused_since(const risk::GateStamp& stamp) const noexcept;
  bool drop_unsent(Pending& pending) noexcept;  // false if the order had nothing left to end
  [[nodiscard]] OpsWindow& bucket(const Uuid& account, std::uint8_t venue) noexcept;
  [[nodiscard]] bool has_token(const OrderDetails& details, Nanos now, std::uint8_t cap) noexcept;
  void take_token(const OrderDetails& details, Nanos now) noexcept;
  [[nodiscard]] bool may_be_at_broker(OrderHandle handle) const noexcept;
  void drain_cancels(CancelAll& out) noexcept;

  Oms* oms_;
  IOrderEventSink* sink_;
  IOrderSender* sender_;
  Venue venue_;
  std::size_t tag_limit_;
  std::size_t capacity_;
  Nanos approval_max_age_;
  Nanos queue_max_age_;
  const ISendLimits* limits_;
  std::vector<Pending> pending_;
  std::vector<OrderHandle> cancels_;  // each order at most once, so reserved at the OMS's capacity
  std::vector<std::uint8_t> asked_;   // per handle: the broker has been asked to cancel it
  std::array<SendBucket, kSendBuckets> buckets_{};
  LatencyHistogram approval_to_send_;
  std::uint64_t stale_approvals_{0};
  std::uint64_t expired_in_queue_{0};
  std::uint64_t expired_after_pause_{0};
  PauseWatch pause_;
};

}  // namespace vyuha::oms

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vyuha/core/error.h"
#include "vyuha/core/price.h"
#include "vyuha/oms/oms.h"
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

// An approval is evaluated and submitted in the same pass of the core thread. Older than this, the kill
// switch or the limits may have changed since: evaluate again. Tight on purpose (fail closed).
inline constexpr Nanos kMaxApprovalAge = kNanosPerSecond / 10;  // 100 ms

// Persist before send (ADR-025). The core thread submits an approved order and polls; nothing here touches
// Postgres. An order goes to the adapter only once its APPROVED event is durable, and is recorded SENT after
// the adapter has it, so in Postgres SENT means "handed over" and APPROVED means "maybe".
class OrderRouter {
 public:
  enum class Recovery : std::uint8_t {
    kResolved,    // matches the broker now
    kUnresolved,  // a mismatch is raised or the sink is full, and the order stays blocked until retried
  };

  OrderRouter(Oms& oms, IOrderEventSink& sink, IOrderSender& sender, Venue venue, std::size_t tag_limit,
              std::size_t capacity);

  // Records APPROVED, with the gate's stamp, and queues the order to send what the gate approved. The order
  // must be NEW, and the approval must be for exactly its details, for this router's venue, unspent and no
  // older than kMaxApprovalAge by the OMS's clock: anything else is kInvalidArgument. A full
  // sink is kCapacityExceeded, the order stays NEW, and the approval is lost with the call: evaluate again.
  // Only this queue reaches the adapter, so an order marked APPROVED some other way (Oms::apply, recovery)
  // is never sent.
  [[nodiscard]] std::expected<void, Error> submit(OrderHandle handle, risk::ApprovedOrder approved) noexcept;
  // Sends what's durable, in order; returns how many it handed to the adapter. Never blocks.
  std::size_t poll() noexcept;
  [[nodiscard]] std::size_t pending() const noexcept { return pending_.size() - head_; }

  // Cancels everything the broker may hold, for the kill switch (M2.7). Nothing here waits on or is refused
  // by the order log: a cancel only reduces risk, so an outage, a full ring or a refused order can't stop
  // one. Unsent orders leave the queue. An APPROVED order is asked too if it may be at the broker. false from
  // the adapter is counted in `failed`: call again, repeats are harmless.
  struct CancelAll {
    std::size_t asked{0};
    std::size_t failed{0};
    std::size_t unrecorded{
        0};  // asked, but the ring was full: the status catches up on the ack or the next call
  };
  CancelAll cancel_all() noexcept;

  // After a restart, for each restored order: find it at the broker by tag and settle it. Never sends or
  // queues anything (the owner's restart rule, ADR-025): an order the broker has is adopted, and an APPROVED
  // order it lacks becomes EXPIRED, because a stale order surfacing after a crash must never fire.
  [[nodiscard]] Recovery recover(OrderHandle handle, IBrokerOrderBook& book);

 private:
  struct Pending {
    OrderHandle handle{0};
    std::uint64_t ticket{0};  // durable() must reach this
    bool sent{false};         // the adapter has it; only recording is left
    risk::ApprovedOrder approved;
  };

  [[nodiscard]] Recovery reconcile_unknown(OrderHandle handle, const BrokerOrderView& broker);
  [[nodiscard]] Recovery catch_up(OrderHandle handle, const BrokerOrderView& broker);
  [[nodiscard]] Recovery settle(OrderHandle handle,
                                const BrokerOrderView* broker);  // nullptr: not at the broker
  [[nodiscard]] Recovery unresolved(OrderHandle handle);
  void drain_refused() noexcept;

  Oms* oms_;
  IOrderEventSink* sink_;
  IOrderSender* sender_;
  Venue venue_;
  std::size_t tag_limit_;
  std::size_t capacity_;
  std::vector<Pending> pending_;
  std::size_t head_{0};
};

}  // namespace vyuha::oms

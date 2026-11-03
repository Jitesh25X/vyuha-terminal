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

namespace vyuha::oms {

// The send side of a broker adapter. M2.2 changes this to take only the gate's ApprovedOrder (invariant 1):
// nothing outside tests implements it before then. send must not block; false means "try again later", with
// nothing sent. broker_tag goes in the broker's tag field.
class IOrderSender {
 public:
  IOrderSender() = default;
  IOrderSender(const IOrderSender&) = delete;
  IOrderSender& operator=(const IOrderSender&) = delete;
  IOrderSender(IOrderSender&&) = delete;
  IOrderSender& operator=(IOrderSender&&) = delete;
  virtual ~IOrderSender() = default;
  [[nodiscard]] virtual bool send(const Order& order, std::string_view broker_tag) noexcept = 0;
  // Asks the broker to cancel. Repeating it is harmless.
  [[nodiscard]] virtual bool cancel(const Order& order) noexcept = 0;
};

struct BrokerOrderView {
  enum class State : std::uint8_t { kOpen, kPartiallyFilled, kFilled, kCancelled, kRejected };
  State state{State::kOpen};
  std::int32_t filled_qty{0};
  Price average_price{};          // NOLINT(readability-redundant-member-init): partly filled by designators
  std::string broker_order_id{};  // NOLINT(readability-redundant-member-init)
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
  // Every order the broker has with this tag.
  [[nodiscard]] virtual std::vector<BrokerOrderView> find_by_tag(std::string_view broker_tag) = 0;
};

// client_order_id in the broker's tag field (ADR-025): its letters and digits, keeping the last `limit`. The
// tail, because the start of a UUIDv7 is a timestamp shared by orders made together.
[[nodiscard]] std::string broker_tag(std::string_view client_order_id, std::size_t limit);

// Persist before send (ADR-025). The core thread submits an approved order and polls; nothing here touches
// Postgres. An order goes to the adapter only once its APPROVED event is durable, and is recorded SENT after
// the adapter has it, so in Postgres SENT means "handed over" and APPROVED means "maybe".
class OrderRouter {
 public:
  enum class Recovery : std::uint8_t {
    kResolved,    // matches the broker now
    kQueued,      // the broker never got it: submitted, and goes out on poll
    kUnresolved,  // a mismatch is raised, and the order stays blocked until someone looks
  };

  OrderRouter(Oms& oms, IOrderEventSink& sink, IOrderSender& sender, std::size_t tag_limit,
              std::size_t capacity);

  // The order's APPROVED event must already be pushed.
  [[nodiscard]] std::expected<void, Error> submit(OrderHandle handle) noexcept;
  // Sends what's durable, in order; returns how many it handed to the adapter. Never blocks.
  std::size_t poll() noexcept;
  [[nodiscard]] std::size_t pending() const noexcept { return pending_.size() - head_; }

  // Cancels everything the broker may hold, for the kill switch (M2.7). Nothing here waits on or is refused
  // by the order log: a cancel only reduces risk, so an outage, a full ring or a refused order can't stop
  // one. Unsent orders leave the queue. false from the adapter is counted in `failed`: call again, repeats
  // are harmless.
  struct CancelAll {
    std::size_t asked{0};
    std::size_t failed{0};
    std::size_t unrecorded{
        0};  // asked, but the ring was full: the status catches up on the ack or the next call
  };
  CancelAll cancel_all() noexcept;

  // After a restart, for each restored order: find it at the broker by tag and settle it. Never sends a
  // second copy: an order the broker has is adopted, and only an APPROVED order the broker lacks is sent.
  [[nodiscard]] Recovery recover(OrderHandle handle, IBrokerOrderBook& book);

 private:
  struct Pending {
    OrderHandle handle{0};
    std::uint64_t ticket{0};  // durable() must reach this
    bool sent{false};         // the adapter has it; only recording is left
  };

  [[nodiscard]] Recovery reconcile_unknown(OrderHandle handle, const BrokerOrderView& broker);
  [[nodiscard]] Recovery catch_up(OrderHandle handle, const BrokerOrderView& broker);
  [[nodiscard]] Recovery unresolved(OrderHandle handle);
  void drain_refused() noexcept;

  Oms* oms_;
  IOrderEventSink* sink_;
  IOrderSender* sender_;
  std::size_t tag_limit_;
  std::size_t capacity_;
  std::vector<Pending> pending_;
  std::size_t head_{0};
};

}  // namespace vyuha::oms

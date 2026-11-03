#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/core/error.h"
#include "vyuha/core/price.h"
#include "vyuha/oms/order_state.h"

namespace vyuha::oms {

// Inline text, so an order or event never allocates on the core thread.
template <std::size_t N>
class FixedString {
 public:
  [[nodiscard]] bool assign(std::string_view s) noexcept {
    if (s.size() > N) {
      return false;
    }
    std::ranges::copy(s, data_.begin());
    size_ = static_cast<std::uint16_t>(s.size());
    return true;
  }
  [[nodiscard]] std::string_view view() const noexcept { return {data_.data(), size_}; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

 private:
  std::array<char, N> data_{};
  std::uint16_t size_{0};
};

struct Uuid {
  std::array<std::uint8_t, 16> bytes{};
  [[nodiscard]] static std::optional<Uuid> parse(std::string_view text) noexcept;
  static constexpr std::size_t kTextSize = 36;
  [[nodiscard]] std::array<char, kTextSize> to_chars() const noexcept;  // no allocation: core thread
  [[nodiscard]] std::string to_string() const;
  auto operator<=>(const Uuid&) const noexcept = default;
};

enum class Side : std::uint8_t { kBuy, kSell };
enum class OrderType : std::uint8_t { kLimit, kMarket, kSl, kSlM };
enum class Product : std::uint8_t { kCnc, kMis, kNrml };
enum class Validity : std::uint8_t { kDay, kIoc };
enum class Source : std::uint8_t { kManual, kStrategy, kAiDraft, kSystem };

// The immutable part of an order: the Postgres `order` row. (Brackets arrive with M2.5.)
struct OrderDetails {
  Uuid id;
  std::optional<Uuid> parent_id;
  FixedString<64> client_order_id;
  Uuid account_id;
  Uuid user_id;
  std::optional<Uuid> strategy_id;
  Source source{Source::kManual};
  std::int64_t instrument_id{0};
  Side side{Side::kBuy};
  std::int32_t qty{0};
  OrderType order_type{OrderType::kLimit};
  Product product{Product::kMis};
  Validity validity{Validity::kDay};
  std::optional<Price> price;
  std::optional<Price> trigger_price;
  std::optional<std::int32_t> market_protection_millipct;  // numeric(6,3), so 2.000 % is 2000
  FixedString<32> tag;
  bool is_paper{true};
  Nanos created_at{0};
};

struct Order {
  OrderDetails details;
  OrderStatus status{OrderStatus::kNew};
  std::int32_t seq{0};  // of the last event
  std::int32_t filled_qty{0};
  FixedString<40> broker_order_id;
  // Restored after a restart. The events are written behind, so the broker may know more than Postgres
  // does: until reconciled this order is never sent or modified.
  bool needs_reconcile{false};
};

// One row of `order_event`, plus what it changes on the `order` row.
struct OrderEventRecord {
  Uuid order_id;
  std::int32_t seq{0};
  std::optional<OrderStatus> from;  // nullopt on the first event, which also inserts the order
  OrderStatus to{OrderStatus::kNew};
  FixedString<32> reason_code;
  std::int32_t filled_qty{0};
  std::int32_t fill_qty{0};  // this event's fill
  Price fill_price;
  FixedString<40> broker_order_id;
  Nanos ts{0};
  OrderDetails details;  // set when `from` is nullopt
};

// Where events go (Postgres via a ring in production). try_push is called on the core thread and must not
// block. A false return leaves the OMS unchanged, so no transition exists without its event.
class IOrderEventSink {
 public:
  IOrderEventSink() = default;
  IOrderEventSink(const IOrderEventSink&) = delete;
  IOrderEventSink& operator=(const IOrderEventSink&) = delete;
  IOrderEventSink(IOrderEventSink&&) = delete;
  IOrderEventSink& operator=(IOrderEventSink&&) = delete;
  virtual ~IOrderEventSink() = default;
  [[nodiscard]] virtual bool try_push(const OrderEventRecord& event) noexcept = 0;
  // False while events aren't reaching storage: nothing is sent to a broker.
  [[nodiscard]] virtual bool healthy() const noexcept { return true; }
  // Events accepted so far, and how many of them are durable. Events are durable in the order they were
  // pushed, so event N is durable once durable() >= N (ADR-025: persist before send).
  [[nodiscard]] virtual std::uint64_t pushed() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t durable() const noexcept = 0;
  // An order storage has refused for good (a duplicate client_order_id that got past intake). Reported before
  // durable() passes its events, so whoever saw them durable can also see this.
  [[nodiscard]] virtual bool try_pop_refused(Uuid& /*order_id*/) noexcept { return false; }
};

using OrderHandle = std::uint32_t;

inline constexpr std::string_view kReconMismatch = "RECON_MISMATCH";
inline constexpr std::string_view kLateAck = "LATE_ACK";

struct Intake {
  OrderHandle handle{0};
  bool existing{
      false};  // the same request came before: nothing new was created, and nothing is to be submitted
};

struct EventDetails {
  std::string_view reason_code{};  // NOLINT(readability-redundant-member-init): partly filled by designators
  std::string_view broker_order_id{};  // NOLINT(readability-redundant-member-init)
  std::int32_t fill_qty{0};            // kPartialFill, kFill: required
  Price fill_price{};                  // NOLINT(readability-redundant-member-init)
  std::int32_t broker_filled_qty{0};   // kReconcilePartial, kReconcileCancelled: the broker's total
};

// What recovery reads back from Postgres.
struct RecoveredOrder {
  OrderDetails details;
  OrderStatus status{OrderStatus::kNew};
  std::int32_t seq{0};
  std::int32_t filled_qty{0};
  std::string broker_order_id;
};

// Order state machine (F-EX-2). Single writer: only the core thread calls it. All storage is reserved up
// front, and nothing here allocates after construction.
class Oms {
 public:
  Oms(IOrderEventSink& sink, const IClock& clock, std::size_t capacity);

  // Idempotent on client_order_id: the same request returns the existing order and records nothing; a
  // different one is kOrderDup (ORDER_DUP). Otherwise records NEW. kInvalidArgument if the order couldn't be
  // stored; kCapacityExceeded if the pool or sink is full. It knows the orders of this run and open ones
  // restored into it; a terminal order of an earlier run is the gateway's check against Postgres (M2.8), and
  // failing that the writer refuses only the duplicate.
  [[nodiscard]] std::expected<Intake, Error> intake(const OrderDetails& details) noexcept;
  // intake, for a caller that doesn't need to tell the two apart.
  [[nodiscard]] std::expected<OrderHandle, Error> create(const OrderDetails& details) noexcept;

  // Applies one event and records it. Errors leave the order unchanged:
  // kIllegalTransition (OMS_ILLEGAL_TRANSITION), kInvalidArgument (bad details),
  // kCapacityExceeded (sink full: retry), kOrderLogUnavailable (kSend while the sink is unhealthy).
  //
  // A fill (kPartialFill, kFill) is a broker fact: it is never refused or dropped, whatever the status. Which
  // of the two it is comes from the totals. In a status with no fill transition, or past the order's qty, it
  // is recorded with reason RECON_MISMATCH: filled_qty is updated (capped at qty; the event holds the real
  // fill), the status stays, and the writer alerts admins. Only a fill of no shares is refused, as garbage.
  [[nodiscard]] std::expected<OrderStatus, Error> apply(OrderHandle handle, OrderEvent event,
                                                        const EventDetails& details = {}) noexcept;

  // Puts back an order read from Postgres, with no new event. It continues from the stored seq.
  [[nodiscard]] std::expected<OrderHandle, Error> restore(const RecoveredOrder& recovered) noexcept;

  // Storage refused this order, so it never goes to a broker: if it hasn't been sent, it ends RISK_REJECTED.
  // No event: storage wouldn't take it. An order already sent keeps its status, and the writer has alerted.
  void refuse(const Uuid& order_id) noexcept;

  // Whether kSend would be allowed now. The router checks this, sends, then calls record_sent.
  [[nodiscard]] bool can_send(OrderHandle handle) const noexcept;
  // Records that the adapter has the order. Only a full sink can refuse it: the order has gone, so the caller
  // retries this and never sends again.
  [[nodiscard]] std::expected<OrderStatus, Error> record_sent(OrderHandle handle) noexcept;
  // Records a mismatch and alerts admins (RECON_MISMATCH), without changing the order.
  [[nodiscard]] std::expected<void, Error> raise_mismatch(OrderHandle handle) noexcept;

  // The broker's order book matches this restored order, so it may be sent or modified again.
  // Records nothing: nothing changed.
  [[nodiscard]] std::expected<void, Error> confirm_reconciled(OrderHandle handle) noexcept;

  [[nodiscard]] const Order& order(OrderHandle handle) const noexcept { return orders_[handle]; }
  [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }
  // The M2 exit criterion: zero of these over a paper day.
  [[nodiscard]] std::uint64_t illegal_transitions() const noexcept { return illegal_; }
  // RECON_MISMATCH events raised, including fills recorded out of turn.
  [[nodiscard]] std::uint64_t recon_mismatches() const noexcept { return recon_mismatches_; }
  [[nodiscard]] std::uint64_t refused_by_storage() const noexcept { return refused_; }

 private:
  [[nodiscard]] std::expected<OrderStatus, Error> illegal(const Order& order, OrderEvent event) noexcept;
  [[nodiscard]] std::optional<OrderHandle> find_client(std::string_view client_order_id) const noexcept;
  void index_client(OrderHandle handle) noexcept;
  [[nodiscard]] std::expected<OrderStatus, Error> record(Order& order, OrderStatus to, std::int32_t filled,
                                                         const EventDetails& details,
                                                         std::string_view reason) noexcept;
  [[nodiscard]] std::expected<OrderStatus, Error> fill(Order& order, const EventDetails& details) noexcept;

  IOrderEventSink* sink_;
  const IClock* clock_;
  std::vector<Order> orders_;
  std::vector<OrderHandle> client_index_;  // open addressing, sized up front; kNoOrder marks an empty slot
  std::size_t capacity_;
  std::uint64_t illegal_{0};
  std::uint64_t recon_mismatches_{0};
  std::uint64_t refused_{0};
};

}  // namespace vyuha::oms

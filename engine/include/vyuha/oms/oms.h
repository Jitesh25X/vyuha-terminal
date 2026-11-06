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
  // False while events aren't reaching storage: Oms refuses to send anything to a broker.
  [[nodiscard]] virtual bool healthy() const noexcept { return true; }
};

using OrderHandle = std::uint32_t;

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

  // Records NEW. kInvalidArgument if the order couldn't be stored; kCapacityExceeded if the pool or sink is
  // full.
  [[nodiscard]] std::expected<OrderHandle, Error> create(const OrderDetails& details) noexcept;

  // Applies one event and records it. Errors leave the order unchanged:
  // kIllegalTransition (OMS_ILLEGAL_TRANSITION), kInvalidArgument (bad details, e.g. an overfill),
  // kCapacityExceeded (sink full: retry), kOrderLogUnavailable (kSend while the sink is unhealthy).
  [[nodiscard]] std::expected<OrderStatus, Error> apply(OrderHandle handle, OrderEvent event,
                                                        const EventDetails& details = {}) noexcept;

  // Puts back an order read from Postgres, with no new event. It continues from the stored seq.
  [[nodiscard]] std::expected<OrderHandle, Error> restore(const RecoveredOrder& recovered) noexcept;

  // The broker's order book matches this restored order, so it may be sent or modified again. A mismatch
  // needs a transition F-EX-2 doesn't list yet (DECISIONS D-17). Records nothing: nothing changed.
  [[nodiscard]] std::expected<void, Error> confirm_reconciled(OrderHandle handle) noexcept;

  [[nodiscard]] const Order& order(OrderHandle handle) const noexcept { return orders_[handle]; }
  [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }
  // The M2 exit criterion: zero of these over a paper day.
  [[nodiscard]] std::uint64_t illegal_transitions() const noexcept { return illegal_; }
  // Fills refused for their details (an overfill, usually a fill lost in a crash). The position may be wrong.
  [[nodiscard]] std::uint64_t refused_fills() const noexcept { return refused_fills_; }

 private:
  [[nodiscard]] std::expected<OrderStatus, Error> illegal(const Order& order, OrderEvent event) noexcept;
  // The order's filled_qty after this event, or why the event's details are refused.
  [[nodiscard]] std::expected<std::int32_t, Error> filled_after(const Order& order, OrderEvent event,
                                                                const EventDetails& details) noexcept;

  IOrderEventSink* sink_;
  const IClock* clock_;
  std::vector<Order> orders_;
  std::size_t capacity_;
  std::uint64_t illegal_{0};
  std::uint64_t refused_fills_{0};
};

}  // namespace vyuha::oms

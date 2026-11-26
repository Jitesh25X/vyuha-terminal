#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/core/error.h"
#include "vyuha/core/price.h"
#include "vyuha/md/last_value.h"
#include "vyuha/md/tick.h"
#include "vyuha/oms/oms.h"
#include "vyuha/oms/order_router.h"
#include "vyuha/risk/approved_order.h"
#include "vyuha/risk/gate_context.h"

namespace vyuha::sim {

// F-EX-4: the paper-trading broker. It is a broker adapter like any other: it takes only the gate's
// ApprovedOrder (invariant 1), runs through the same Oms and OrderRouter as a live adapter, and is the only
// broker tests, CI and dev may use (invariant 3). It never builds an ApprovedOrder and never calls the gate.
//
// Core thread only. Every hot-path function is noexcept and allocates nothing after construction: orders,
// history, the event queue and the indexes are all sized up front. Deterministic: the same ticks, orders and
// seed give the same events. Money is Price (int64), never a float.
//
// Known simplifications of M2.5a (each belongs to a later task):
//  - Fills are for the whole remaining quantity. There are no partial fills (kPartialFill is never emitted).
//  - No end-of-day expiry of DAY orders and no MIS auto square-off (M2.6, M2.8).
//  - State is in memory and not persisted. After an engine restart the SimBroker holds nothing, so OMS
//    recovery (ADR-025) finds its working orders absent. Restoring sim state from the order log is M2.6/M2.8.
//  - No modify. No queue-position model beyond the touch probability below.
//  - A stop already through its trigger when it is sent triggers at once. That is a modelling choice: a real
//    broker may reject it (the gate's R-TRIGGER-SIDE normally prevents it).
//  - `history` is how many ended orders are remembered (find_by_tag, a late cancel). If an OMS is ever
//    restored against a sim that survives, it must cover a session's ended orders.

inline constexpr std::uint64_t kDefaultSeed = 0x5DEECE66DULL;

// Static reason texts carried by events (and recorded by the OMS as the event's reason_code).
inline constexpr std::string_view kReasonInvalid = "SIM_INVALID";         // a malformed order
inline constexpr std::string_view kReasonTooLate = "SIM_TOO_LATE";        // cancel of an order already ended
inline constexpr std::string_view kReasonIocRemainder = "IOC_REMAINDER";  // what an IOC did not fill

struct SimConfig {
  // Working orders at once. A full table refuses send (false). 1 to 4,194,304. Default 4,096.
  std::size_t max_orders{4096};
  // Ended orders kept, as a ring, so find_by_tag and a late cancel can still see them. 1 to 4,194,304.
  // Default 4,096.
  std::size_t history{4096};
  // Market and SL-M orders fill this many ticks past the opposite best. 0 to 10,000. Default 1.
  std::int32_t slippage_ticks{1};
  // The chance, in thousandths, that a limit order whose price equals the LTP (a touch) fills. 0 to 1,000.
  // Default 0: a touch never fills, only a trade-through does.
  std::uint32_t touch_fill_permille{0};
  // Seed of the generator behind the touch draws. Default kDefaultSeed.
  std::uint64_t seed{kDefaultSeed};
  // Events waiting for the core loop to drain (pop_event or apply_events). 4 to 16,777,216. Default 8,192.
  // When full the SimBroker stops matching, so a fill is never lost.
  std::size_t event_capacity{8192};
};

// The generator behind the touch draws, public so a test can reproduce them. SplitMix64: any seed works,
// including 0, and the same seed gives the same sequence.
class SimRng {
 public:
  explicit SimRng(std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  // One draw: true with probability permille/1000.
  [[nodiscard]] bool hit(std::uint32_t permille) noexcept { return (next() % 1000U) < permille; }

 private:
  std::uint64_t state_;
};

// What the SimBroker tells the OMS, queued for the core loop to drain. kPartialFill is never used.
// NOLINTBEGIN(readability-redundant-member-init): see OrderDetails
struct SimEvent {
  oms::OrderEvent kind{oms::OrderEvent::kBrokerAck};
  oms::FixedString<64> client_order_id{};
  oms::Uuid order_id{};
  oms::FixedString<40> broker_order_id{};  // "SIM-<n>"; empty on a reject, which never got one
  std::int32_t fill_qty{0};
  Price fill_price{};
  oms::FixedString<40> broker_trade_id{};  // "SIMT-<n>", on a fill
  std::string_view reason_code{};          // static text
};
// NOLINTEND(readability-redundant-member-init)

struct SimStats {
  std::uint64_t queue_full{0};          // send, cancel or a tick stopped because the event queue was full
  std::uint64_t refused_live{0};        // a live order handed to the SimBroker
  std::uint64_t refused_spent{0};       // a spent approval
  std::uint64_t refused_full{0};        // the order table was full
  std::uint64_t refused_instrument{0};  // not in the instrument table, or unusable tick size
  std::uint64_t refused_tag{0};         // an unusable tag, or one held by a different client_order_id
  std::uint64_t rejected_invalid{0};    // SIM_INVALID
  std::uint64_t cancel_unknown{0};
  std::uint64_t fills{0};
  // Counted by apply_events:
  std::uint64_t unknown_orders{0};         // an event for an order the OMS doesn't know
  std::uint64_t apply_errors{0};           // Oms::apply refused the event (not a full sink)
  std::uint64_t apply_retries{0};          // the sink was full: the event stayed queued
  std::uint64_t stale_cancel_rejects{0};   // a cancel reject the OMS had no cancel pending for (it had ended)
  std::uint64_t stale_cancel_confirms{0};  // a repeated cancel confirm for an order the OMS had ended
  // An event whose order the OMS knows by client_order_id but under another order id. Not applied, and not
  // dropped silently: an alert should read this, and unknown_orders, since a fill for an order the OMS
  // doesn't hold, or holds as another, is a fact someone must look at.
  std::uint64_t mismatched_orders{0};
};

class SimBroker;
// Drains the SimBroker's events into the OMS (Oms::apply), looking each order up by client_order_id. An event
// for an order the OMS doesn't know is counted and skipped. A full sink (kCapacityExceeded) leaves the event
// queued for the next call, so a fill is never dropped; any other refusal is counted. A cancel reject for an
// order the OMS has already ended (the fill or the cancel came first) is a race and not a transition: it is
// counted (stale_cancel_rejects) and not applied, as is one for an order with no cancel pending. An event
// whose order id differs from the OMS's order under that client_order_id is counted (mismatched_orders) and
// not applied. The router records no CANCEL_REQUEST for a SENT order, so a cancel confirm for an order in
// SENT, OPEN or PARTIALLY_FILLED is applied as kBrokerCancel (legal from all three); in CANCEL_PENDING it is
// kCancelConfirm; for an order already ended it is counted (stale_cancel_confirms). While the OMS still has
// the order as APPROVED (the router's record_sent is being retried) the pump stops and keeps the event
// queued, counted in apply_retries, so an ack is never applied before SENT. Returns how many events the OMS
// took.
std::size_t apply_events(SimBroker& broker, oms::Oms& oms) noexcept;

class SimBroker final : public oms::IOrderSender, public oms::IBrokerOrderBook {
 public:
  // Only SimBroker can make one: create() is the one way in, so a bad config can't be built.
  class PassKey {
   private:
    friend class SimBroker;
    PassKey() = default;
  };

  // Borrowed references must outlive the SimBroker. The config is validated: a value out of the ranges
  // documented on SimConfig is kInvalidArgument.
  [[nodiscard]] static std::expected<SimBroker, Error> create(const risk::InstrumentTable& instruments,
                                                              const md::LastValueBook& book,
                                                              const IClock& clock,
                                                              const SimConfig& config = {});
  static std::expected<SimBroker, Error> create(const risk::InstrumentTable&& instruments,
                                                const md::LastValueBook& book, const IClock& clock,
                                                const SimConfig& config) = delete;
  static std::expected<SimBroker, Error> create(const risk::InstrumentTable& instruments,
                                                const md::LastValueBook&& book, const IClock& clock,
                                                const SimConfig& config) = delete;
  static std::expected<SimBroker, Error> create(const risk::InstrumentTable& instruments,
                                                const md::LastValueBook& book, const IClock&& clock,
                                                const SimConfig& config) = delete;

  // Use create(). Public only so std::expected can build it in place.
  SimBroker(PassKey key, const risk::InstrumentTable& instruments, const md::LastValueBook& book,
            const IClock& clock, const SimConfig& config);

  // IOrderSender. false, with nothing created, for: a spent approval; a live order (this broker takes paper
  // orders only, even if a router were wired to it wrongly); a full order table; a full event queue; an
  // instrument not in the table (or with no tick size, for a type that needs one); a tag that is empty, too
  // long, or held by a different client_order_id.
  //
  // A tag already held for the same client_order_id returns true and creates nothing: the order has already
  // been handed over. A malformed order (qty <= 0; LIMIT or SL with no price, or a price <= 0; SL or SL-M
  // with no trigger, or a trigger <= 0) is accepted by send and then ended by a kBrokerReject event with
  // reason SIM_INVALID, never with an ack too. Otherwise a kBrokerAck follows, with id SIM-<n>, and the order
  // is evaluated once against the instrument's last tick, if that is no older than risk::kLtpMaxAge.
  [[nodiscard]] bool send(const risk::ApprovedOrder& approved, std::string_view broker_tag) noexcept override;
  // Looked up by client_order_id (the order's tag is derived from it), and, when the OMS already has a
  // broker_order_id, that must match. Working: removed, kCancelConfirm, true. Already cancelled:
  // kCancelConfirm again (idempotent, so an OMS order waiting in CANCEL_PENDING closes). Filled or rejected
  // (still in the history): kCancelReject SIM_TOO_LATE, true. Unknown: false. A full event queue: false,
  // retry later.
  [[nodiscard]] bool cancel(const oms::Order& order) noexcept override;

  // IBrokerOrderBook. Every order with the tag, working or in the history. An unknown tag is an empty list.
  // The book can always be read, so this never returns an error. Not for the core thread's hot path.
  [[nodiscard]] std::expected<std::vector<oms::BrokerOrderView>, Error> find_by_tag(
      std::string_view broker_tag) override;

  // Call for every tick of every instrument. Only that instrument's working orders are visited, in the order
  // they were sent. A tick with ltp <= 0 is ignored. Returns false if it stopped because the event queue was
  // full: the orders not yet visited wait for the next tick, and this one is not replayed (a caller that
  // wants it re-evaluated calls on_tick again). Callers pass only live ticks: a tick is fresh by definition,
  // and its age is not checked (only send checks the age of the book's last tick).
  //
  // Fills (the whole remaining quantity each):
  //  - LIMIT BUY fills at its price when ltp < price (traded through). At ltp == price (a touch) it fills
  //    with probability touch_fill_permille/1000, one draw per touch evaluation. SELL mirrors it.
  //  - MARKET BUY fills at the ask plus slippage_ticks * tick_size, SELL at the bid minus that. With no
  //    quote on the opposite side the LTP stands in for it.
  //  - A marketable LIMIT (BUY with price >= ask > 0, SELL with price <= bid, bid > 0) fills at the opposite
  //    best, with no slippage.
  //  - SL / SL-M: BUY triggers when ltp >= trigger, SELL when ltp <= trigger, then SL-M becomes a MARKET
  //    order and SL a LIMIT at its price. One that can't fill on that tick keeps working as the converted
  //    order.
  //  - IOC is evaluated once, on send; whatever isn't filled is cancelled (kBrokerCancel, IOC_REMAINDER).
  bool on_tick(const md::Tick& tick) noexcept;

  // The events for the OMS, oldest first. pop_event copies one out. peek_event/discard_event let a caller
  // look, try, and keep the event queued if it can't be taken yet.
  [[nodiscard]] bool pop_event(SimEvent& out) noexcept;
  [[nodiscard]] const SimEvent* peek_event() const noexcept;
  void discard_event() noexcept;
  [[nodiscard]] std::size_t events_pending() const noexcept { return ev_count_; }

  [[nodiscard]] std::size_t working() const noexcept { return working_; }
  [[nodiscard]] const SimStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const SimConfig& config() const noexcept { return config_; }

 private:
  friend std::size_t apply_events(SimBroker& broker, oms::Oms& oms) noexcept;

  enum class State : std::uint8_t { kFree, kWorking, kFilled, kCancelled, kRejected };
  static constexpr std::int32_t kNone = -1;

  // NOLINTBEGIN(readability-redundant-member-init)
  struct Rec {
    oms::FixedString<40> tag{};
    oms::FixedString<64> client_order_id{};
    oms::Uuid order_id{};
    oms::FixedString<40> broker_order_id{};
    std::int64_t instrument_id{0};
    Price tick_size{};
    oms::Side side{oms::Side::kBuy};
    oms::OrderType type{oms::OrderType::kLimit};
    oms::Validity validity{oms::Validity::kDay};
    State state{State::kFree};
    bool triggered{false};  // a stop whose trigger has been hit
    std::int32_t qty{0};
    std::int32_t filled_qty{0};
    Price average{};
    std::optional<Price> price{};
    std::optional<Price> trigger{};
    std::int32_t tag_next{kNone};
    std::int32_t cid_next{kNone};
    std::int32_t inst_next{kNone};  // also the free list's link
    std::int32_t inst_prev{kNone};
  };
  // NOLINTEND(readability-redundant-member-init)

  // A hash table of intrusive chains with a tail pointer, so appending keeps first-in-first-out order.
  struct Chain {
    explicit Chain(std::size_t buckets) : head(buckets, kNone), tail(buckets, kNone), mask(buckets - 1) {}
    std::vector<std::int32_t> head;
    std::vector<std::int32_t> tail;
    std::size_t mask;
  };

  template <auto Next>
  void link(Chain& chain, std::size_t bucket, std::int32_t idx) noexcept;
  template <auto Next>
  void unlink(Chain& chain, std::size_t bucket, std::int32_t idx) noexcept;

  [[nodiscard]] std::size_t tag_bucket(std::string_view tag) const noexcept;
  [[nodiscard]] std::size_t cid_bucket(std::string_view cid) const noexcept;
  [[nodiscard]] std::size_t inst_bucket(std::int64_t instrument_id) const noexcept;
  [[nodiscard]] std::int32_t find_tag(std::string_view tag) const noexcept;
  [[nodiscard]] std::int32_t find_cid(std::string_view cid) const noexcept;

  [[nodiscard]] std::size_t events_free() const noexcept { return events_.size() - ev_count_; }
  // The caller has checked events_free().
  void emit(oms::OrderEvent kind, const Rec& rec, std::int32_t fill_qty, Price fill_price,
            std::string_view trade_id, std::string_view reason) noexcept;

  [[nodiscard]] std::int32_t claim_history() noexcept;
  void retire(std::int32_t working_idx, State to) noexcept;
  void link_inst(std::size_t bucket, std::int32_t idx) noexcept;
  void unlink_inst(std::size_t bucket, std::int32_t idx) noexcept;
  void complete(std::int32_t idx, Price price) noexcept;
  [[nodiscard]] std::optional<Price> match(Rec& rec, const md::Tick& tick) noexcept;
  [[nodiscard]] const md::Tick* usable_tick(std::int64_t instrument_id) const noexcept;

  const risk::InstrumentTable* instruments_;
  const md::LastValueBook* book_;
  const IClock* clock_;
  SimConfig config_;
  SimRng rng_;
  std::vector<Rec> recs_;  // [0, max_orders): working; then the history ring
  Chain tags_;
  Chain cids_;
  Chain insts_;  // working orders only, by instrument
  std::int32_t free_head_{kNone};
  std::size_t hist_pos_{0};
  std::size_t working_{0};
  std::vector<SimEvent> events_;
  std::size_t ev_head_{0};
  std::size_t ev_count_{0};
  std::uint64_t order_seq_{0};
  std::uint64_t trade_seq_{0};
  SimStats stats_;
};

}  // namespace vyuha::sim

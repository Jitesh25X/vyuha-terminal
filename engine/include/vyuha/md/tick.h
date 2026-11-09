#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "vyuha/core/clock.h"
#include "vyuha/core/price.h"

namespace vyuha::md {

using InstrumentId = std::uint64_t;  // instrument.id (bigserial); 0 = none

enum class Source : std::uint8_t { kUnknown = 0, kZerodha = 1, kUpstox = 2, kSim = 3 };

inline constexpr std::size_t kDepthLevels = 5;

struct DepthLevel {
  Price price;
  std::uint32_t qty{0};
  std::uint32_t orders{0};
};

// Canonical tick (mirrors vyuha.marketdata.v1.Tick). Decoders on IO threads fill it from broker
// frames; the core stamps `seq`. Trivially copyable so it moves through SpscRing by plain copy.
// `volume` is the exchange's cumulative day volume; `ltp` is zero when the feed carries no trade.
struct Tick {
  InstrumentId instrument_id{0};
  Nanos exchange_ts{0};
  Nanos recv_ts{0};
  // IClock::monotonic when the frame arrived: the only stamp LTP age is measured from (D-26). Meaningless
  // outside this process, so it is never published or stored.
  Nanos recv_mono{0};
  // When this feed last saw the instrument trade; 0 = unknown. Decoders set it when the broker sends
  // one (Upstox ltt, Kite full mode); the core's last-value book infers it otherwise and carries it forward.
  Nanos last_trade_ts{0};
  std::uint64_t seq{0};
  Source source{Source::kUnknown};
  std::uint32_t feed_id{0};  // session whose feed received it (entitlement routing, ADR-009)
  Price ltp;
  std::uint32_t ltq{0};
  std::uint64_t volume{0};
  std::uint64_t oi{0};
  Price open;
  Price high;
  Price low;
  Price prev_close;
  Price avg_price;
  Price bid;
  Price ask;
  std::uint32_t bid_qty{0};
  std::uint32_t ask_qty{0};
  std::array<DepthLevel, kDepthLevels> bids{};
  std::array<DepthLevel, kDepthLevels> asks{};
};
static_assert(std::is_trivially_copyable_v<Tick>);

enum class BarInterval : std::uint8_t { k1s = 0, k1m = 1 };

[[nodiscard]] constexpr Nanos interval_nanos(BarInterval i) noexcept {
  return i == BarInterval::k1s ? kNanosPerSecond : kNanosPerMinute;
}

// OHLCV bar (mirrors vyuha.marketdata.v1.Bar). `ts` is the bar's open time (UTC ns).
struct Bar {
  InstrumentId instrument_id{0};
  BarInterval interval{BarInterval::k1m};
  Nanos ts{0};
  Price open;
  Price high;
  Price low;
  Price close;
  std::uint64_t volume{0};
  std::uint64_t oi{0};
  bool final{false};
};
static_assert(std::is_trivially_copyable_v<Bar>);

// Where the core sends normalized ticks and finished bars (NATS publisher ring later). Called on
// the core thread; implementations must not block or allocate per call.
class IMdSink {
 public:
  IMdSink() = default;
  IMdSink(const IMdSink&) = delete;
  IMdSink& operator=(const IMdSink&) = delete;
  IMdSink(IMdSink&&) = delete;
  IMdSink& operator=(IMdSink&&) = delete;
  virtual ~IMdSink() = default;
  virtual void on_tick(const Tick& tick) noexcept = 0;
  virtual void on_bar(const Bar& bar) noexcept = 0;
};

}  // namespace vyuha::md

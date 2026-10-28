#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/core/price.h"
#include "vyuha/md/tick.h"

namespace vyuha::broker::upstox {

// Upstox Market Data Feed V3 (protobuf `FeedResponse`, schema third_party/upstox/MarketDataFeed.proto
// from assets.upstox.com, fetched 2026-10-29). Decoded straight from the protobuf wire format with
// string_views into the frame: no allocation per frame or tick (engine hot-path rule).
//
// FeedResponse { Type type=1; map<string,Feed> feeds=2; int64 currentTs=3 (ms); MarketInfo=4 }
// Feed oneof { LTPC=1 | FullFeed=2 {marketFF=1 | indexFF=2} | FirstLevelWithGreeks=3 }
// Prices are doubles in rupees: converted to exact Price units (round to nearest 1/10 000 rupee).

// Upstox instrument_key ("NSE_FO|50001") → canonical instrument id. Read-only after construction.
class KeyMap {
 public:
  KeyMap() = default;
  explicit KeyMap(std::vector<std::pair<std::string, md::InstrumentId>> entries);
  [[nodiscard]] md::InstrumentId find(std::string_view key) const noexcept;  // 0 = unknown
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

 private:
  std::vector<std::pair<std::string, md::InstrumentId>> entries_;  // sorted by key
};

enum class FrameType : std::uint8_t { kInitialFeed = 0, kLiveFeed = 1, kMarketInfo = 2, kUnknown = 255 };

struct DecodeStats {
  std::uint64_t frames{0};
  std::uint64_t market_info{0};
  std::uint64_t feeds{0};
  std::uint64_t ticks{0};
  std::uint64_t unknown_key{0};
  std::uint64_t bad_price{0};  // NaN/inf/negative/out of range: feed dropped
  std::uint64_t malformed{0};  // invalid wire format: rest of the frame dropped
};

// Decodes one FeedResponse frame, calling emit(const md::Tick&) per known instrument.
// Returns the frame type. Never reads outside `frame`.
template <class Emit>
FrameType decode_frame(std::span<const std::uint8_t> frame, const KeyMap& keys, Nanos recv_ts,
                       DecodeStats& stats,
                       Emit emit) noexcept(std::is_nothrow_invocable_v<Emit&, const md::Tick&>);

// --- implementation detail -------------------------------------------------------------------
namespace detail {

// Visits feeds of a frame: header pass then entries. Defined in feed_parser.cc; templates below
// forward through a small non-template trampoline so the decoder body stays out of the header.
using TickFn = void (*)(void* ctx, const md::Tick& tick);
FrameType decode_frame_impl(std::span<const std::uint8_t> frame, const KeyMap& keys, Nanos recv_ts,
                            DecodeStats& stats, TickFn fn, void* ctx) noexcept;

}  // namespace detail

template <class Emit>
FrameType decode_frame(std::span<const std::uint8_t> frame, const KeyMap& keys, Nanos recv_ts,
                       DecodeStats& stats,
                       Emit emit) noexcept(std::is_nothrow_invocable_v<Emit&, const md::Tick&>) {
  return detail::decode_frame_impl(
      frame, keys, recv_ts, stats, [](void* ctx, const md::Tick& t) { (*static_cast<Emit*>(ctx))(t); },
      &emit);
}

}  // namespace vyuha::broker::upstox

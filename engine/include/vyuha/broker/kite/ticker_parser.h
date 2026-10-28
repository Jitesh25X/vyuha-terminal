#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "vyuha/core/clock.h"
#include "vyuha/md/tick.h"

namespace vyuha::broker::kite {

// Kite Ticker binary protocol (docs: kite.trade/docs/connect/v3/websocket, verified 2026-10-29;
// byte order big-endian per Zerodha's pykiteconnect). A frame is
//   u16 packet_count, then packet_count × (u16 length, `length` bytes).
// A frame shorter than 2 bytes is a heartbeat. Packet layouts by length:
//    8  LTP (any instrument)            token, ltp
//   28  index quote / 32 index full     token, ltp, high, low, open, close, change[, exch_ts]
//   44  quote / 184 full                token, ltp, ltq, avg, volume, buy_qty, sell_qty, open,
//                                       high, low, close[, last_trade_ts, oi, oi_hi, oi_lo,
//                                       exch_ts, 5 bid + 5 ask depth × (qty, price, u16 orders, pad)]
// The low byte of the instrument token is Kite's segment. Prices are paise for every segment we
// trade (NSE, NFO, BSE, BFO, indices); currency/commodity segments are rejected.

enum class Segment : std::uint8_t {
  kNse = 1,
  kNfo = 2,
  kCds = 3,
  kBse = 4,
  kBfo = 5,
  kBcd = 6,
  kMcx = 7,
  kMcxsx = 8,
  kIndices = 9,
  kNco = 12,
};

[[nodiscard]] constexpr Segment segment_of(std::uint32_t token) noexcept {
  return static_cast<Segment>(token & 0xFFU);
}

// Kite instrument_token → canonical instrument id. Built once per subscription set and read-only
// afterwards, so the IO thread can use it without locks.
class TokenMap {
 public:
  TokenMap() = default;
  explicit TokenMap(std::vector<std::pair<std::uint32_t, md::InstrumentId>> entries);
  [[nodiscard]] md::InstrumentId find(std::uint32_t token) const noexcept;  // 0 = unknown
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

 private:
  std::vector<std::pair<std::uint32_t, md::InstrumentId>> entries_;  // sorted by token
};

struct ParseStats {
  std::uint64_t frames{0};
  std::uint64_t heartbeats{0};
  std::uint64_t packets{0};
  std::uint64_t ticks{0};
  std::uint64_t unknown_token{0};        // not in the TokenMap (e.g. unsubscribed meanwhile)
  std::uint64_t unsupported_segment{0};  // currency/commodity
  std::uint64_t malformed{0};            // bad lengths or truncated frames
};

enum class PacketResult : std::uint8_t { kTick, kUnknownToken, kUnsupportedSegment, kMalformed };

// Decodes one packet into `out` (overwriting it). Never reads outside `packet`.
[[nodiscard]] PacketResult parse_packet(std::span<const std::uint8_t> packet, const TokenMap& tokens,
                                        Nanos recv_ts, md::Tick& out) noexcept;

// Splits a frame and calls `emit(const md::Tick&)` per decoded tick. A malformed frame stops at
// the first inconsistency (earlier packets are still emitted). No allocation.
template <class Emit>
void parse_frame(std::span<const std::uint8_t> frame, const TokenMap& tokens, Nanos recv_ts,
                 ParseStats& stats, Emit emit) noexcept(std::is_nothrow_invocable_v<Emit&, const md::Tick&>) {
  ++stats.frames;
  if (frame.size() < 2) {
    ++stats.heartbeats;
    return;
  }
  const auto u16 = [&frame](std::size_t at) noexcept {
    return static_cast<std::size_t>((static_cast<unsigned>(frame[at]) << 8U) | frame[at + 1]);
  };
  const std::size_t count = u16(0);
  std::size_t at = 2;
  md::Tick tick;
  for (std::size_t i = 0; i < count; ++i) {
    if (at + 2 > frame.size()) {
      ++stats.malformed;
      return;
    }
    const std::size_t len = u16(at);
    at += 2;
    if (len > frame.size() - at) {
      ++stats.malformed;
      return;
    }
    ++stats.packets;
    switch (parse_packet(frame.subspan(at, len), tokens, recv_ts, tick)) {
      case PacketResult::kTick:
        ++stats.ticks;
        emit(std::as_const(tick));
        break;
      case PacketResult::kUnknownToken:
        ++stats.unknown_token;
        break;
      case PacketResult::kUnsupportedSegment:
        ++stats.unsupported_segment;
        break;
      case PacketResult::kMalformed:
        ++stats.malformed;
        break;
    }
    at += len;
  }
}

}  // namespace vyuha::broker::kite

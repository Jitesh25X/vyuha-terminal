#include "vyuha/broker/kite/ticker_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace vyuha::broker::kite {

namespace {

constexpr std::size_t kLtp = 8;
constexpr std::size_t kIndexQuote = 28;
constexpr std::size_t kIndexFull = 32;
constexpr std::size_t kQuote = 44;
constexpr std::size_t kFull = 184;
constexpr std::size_t kDepthOffset = 64;
constexpr std::size_t kDepthEntry = 12;

// Big-endian readers; callers guarantee `at + N <= p.size()` from the packet-length switch.
std::uint32_t be32(std::span<const std::uint8_t> p, std::size_t at) noexcept {
  return (static_cast<std::uint32_t>(p[at]) << 24U) | (static_cast<std::uint32_t>(p[at + 1]) << 16U) |
         (static_cast<std::uint32_t>(p[at + 2]) << 8U) | static_cast<std::uint32_t>(p[at + 3]);
}

std::uint16_t be16(std::span<const std::uint8_t> p, std::size_t at) noexcept {
  return static_cast<std::uint16_t>((static_cast<unsigned>(p[at]) << 8U) | p[at + 1]);
}

Price paise(std::span<const std::uint8_t> p, std::size_t at) noexcept {
  return Price::from_paise(static_cast<std::int64_t>(be32(p, at)));
}

Nanos seconds(std::span<const std::uint8_t> p, std::size_t at) noexcept {
  return static_cast<Nanos>(be32(p, at)) * kNanosPerSecond;
}

void parse_depth(std::span<const std::uint8_t> p, md::Tick& out) noexcept {
  for (std::size_t i = 0; i < md::kDepthLevels; ++i) {
    for (const bool ask : {false, true}) {
      const std::size_t at = kDepthOffset + ((i + (ask ? md::kDepthLevels : 0)) * kDepthEntry);
      md::DepthLevel& lvl = *((ask ? out.asks.data() : out.bids.data()) + i);  // i < kDepthLevels
      lvl.qty = be32(p, at);
      lvl.price = paise(p, at + 4);
      lvl.orders = be16(p, at + 8);
    }
  }
  out.bid = out.bids[0].price;
  out.bid_qty = out.bids[0].qty;
  out.ask = out.asks[0].price;
  out.ask_qty = out.asks[0].qty;
}

bool supported(Segment s) noexcept {
  switch (s) {
    case Segment::kNse:
    case Segment::kNfo:
    case Segment::kBse:
    case Segment::kBfo:
    case Segment::kIndices:
      return true;
    default:
      return false;
  }
}

}  // namespace

TokenMap::TokenMap(std::vector<std::pair<std::uint32_t, md::InstrumentId>> entries)
    : entries_(std::move(entries)) {
  std::ranges::sort(entries_);
  const auto dup = std::ranges::unique(entries_, {}, &std::pair<std::uint32_t, md::InstrumentId>::first);
  entries_.erase(dup.begin(), dup.end());
}

md::InstrumentId TokenMap::find(std::uint32_t token) const noexcept {
  const auto it =
      std::ranges::lower_bound(entries_, token, {}, &std::pair<std::uint32_t, md::InstrumentId>::first);
  return it != entries_.end() && it->first == token ? it->second : 0;
}

PacketResult parse_packet(std::span<const std::uint8_t> p, const TokenMap& tokens, Nanos recv_ts,
                          md::Tick& out) noexcept {
  const std::size_t n = p.size();
  if (n != kLtp && n != kIndexQuote && n != kIndexFull && n != kQuote && n != kFull) {
    return PacketResult::kMalformed;
  }
  const std::uint32_t token = be32(p, 0);
  const Segment seg = segment_of(token);
  if (!supported(seg)) {
    return PacketResult::kUnsupportedSegment;
  }
  const md::InstrumentId id = tokens.find(token);
  if (id == 0) {
    return PacketResult::kUnknownToken;
  }
  const bool index = seg == Segment::kIndices;
  if (index ? (n == kQuote || n == kFull) : (n == kIndexQuote || n == kIndexFull)) {
    return PacketResult::kMalformed;  // index layouts only for indices, and vice versa
  }

  out = md::Tick{};
  out.instrument_id = id;
  out.source = md::Source::kZerodha;
  out.recv_ts = recv_ts;
  out.exchange_ts = recv_ts;  // LTP/quote packets carry no exchange time
  out.ltp = paise(p, 4);
  if (n == kLtp) {
    return PacketResult::kTick;
  }
  if (index) {
    out.high = paise(p, 8);
    out.low = paise(p, 12);
    out.open = paise(p, 16);
    out.prev_close = paise(p, 20);
    if (n == kIndexFull) {
      out.exchange_ts = seconds(p, 28);
    }
    return PacketResult::kTick;
  }
  out.ltq = be32(p, 8);
  out.avg_price = paise(p, 12);
  out.volume = be32(p, 16);
  out.open = paise(p, 28);
  out.high = paise(p, 32);
  out.low = paise(p, 36);
  out.prev_close = paise(p, 40);
  if (n == kFull) {
    out.last_trade_ts = seconds(p, 44);  // last-traded timestamp (0 when it hasn't traded)
    out.oi = be32(p, 48);
    out.exchange_ts = seconds(p, 60);
    parse_depth(p, out);
  }
  return PacketResult::kTick;
}

}  // namespace vyuha::broker::kite

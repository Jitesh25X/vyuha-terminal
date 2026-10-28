// libFuzzer target for the Upstox Market Data Feed V3 protobuf wire decoder.
//   make fuzz-upstox FUZZ_SECONDS=600

#include <cstddef>
#include <cstdint>
#include <span>

#include "vyuha/broker/upstox/feed_parser.h"

namespace {

const vyuha::broker::upstox::KeyMap& keys() {
  static const vyuha::broker::upstox::KeyMap map(
      {{"NSE_FO|61471", 1}, {"NSE_INDEX|Nifty 50", 2}, {"NSE_EQ|INE002A01018", 3}});
  return map;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  vyuha::broker::upstox::DecodeStats stats;
  std::uint64_t sum = 0;
  bool negative = false;
  vyuha::broker::upstox::decode_frame(std::span<const std::uint8_t>(data, size), keys(), 1, stats,
                                      [&](const vyuha::md::Tick& t) noexcept {
                                        sum += static_cast<std::uint64_t>(t.ltp.units()) + t.volume;
                                        negative = negative || t.ltp.units() < 0 || t.bid.units() < 0;
                                      });
  // Invariants for any input: no more ticks than feeds, and no negative/invalid price escapes.
  if (stats.ticks > stats.feeds || negative) {
    __builtin_trap();
  }
  return sum == 0xDEADBEEF ? 1 : 0;
}

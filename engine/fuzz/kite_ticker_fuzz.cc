// libFuzzer target for the Kite Ticker binary parser (engine/CLAUDE.md: every broker adapter has a
// fuzz target for its parser). Built by the `fuzz` preset only (clang with -fsanitize=fuzzer).
//   make fuzz-kite FUZZ_SECONDS=600

#include <cstddef>
#include <cstdint>
#include <span>

#include "vyuha/broker/kite/ticker_parser.h"

namespace {

const vyuha::broker::kite::TokenMap& tokens() {
  // Tokens from the fixtures plus a few per segment so known-token paths are reachable.
  static const vyuha::broker::kite::TokenMap map({{(61471U << 8U) | 2U, 1},
                                                  {(2885U << 8U) | 1U, 2},
                                                  {256265, 3},
                                                  {(1U << 8U) | 4U, 4},
                                                  {(1U << 8U) | 5U, 5},
                                                  {(1U << 8U) | 9U, 6}});
  return map;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  vyuha::broker::kite::ParseStats stats;
  std::uint64_t sum = 0;
  vyuha::broker::kite::parse_frame(
      std::span<const std::uint8_t>(data, size), tokens(), 1, stats,
      [&sum](const vyuha::md::Tick& t) { sum += static_cast<std::uint64_t>(t.ltp.units()) + t.volume; });
  // Invariants that must hold for any input.
  if (stats.ticks > stats.packets || stats.packets > size / 2) {
    __builtin_trap();
  }
  return sum == 0xDEADBEEF ? 1 : 0;  // keep `sum` observable
}

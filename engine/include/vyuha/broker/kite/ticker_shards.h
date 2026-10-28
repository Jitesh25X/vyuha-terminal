#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "vyuha/core/error.h"

namespace vyuha::broker::kite {

// Kite Ticker limits (kite.trade/docs/connect/v3/websocket, verified 2026-10-29). See README.md.
struct TickerLimits {
  std::size_t max_per_connection{3'000};
  std::size_t max_connections{3};
};

enum class Mode : std::uint8_t { kLtp, kQuote, kFull };

struct Subscription {
  std::uint32_t token{0};
  Mode mode{Mode::kQuote};
};

struct Shard {
  std::vector<Subscription> subs;
};

// Splits subscriptions over the fewest connections without exceeding either limit (F-MD-2 AC1).
// Duplicate tokens keep the richest mode. kCapacityExceeded when more than
// max_per_connection × max_connections distinct tokens are requested.
[[nodiscard]] std::expected<std::vector<Shard>, Error> plan_shards(std::span<const Subscription> subs,
                                                                   const TickerLimits& limits = {});

// Text frames that (re)establish a shard after connect: one subscribe, then one mode message per
// mode in use, e.g. {"a":"subscribe","v":[408065]} and {"a":"mode","v":["full",[408065]]}.
[[nodiscard]] std::vector<std::string> subscribe_messages(const Shard& shard);

}  // namespace vyuha::broker::kite

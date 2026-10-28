#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "vyuha/core/error.h"

namespace vyuha::broker::upstox {

// Upstox Market Data Feed V3 subscription modes and limits
// (upstox.com/developer/api-documentation/v3/get-market-data-feed, verified 2026-10-29).
enum class Mode : std::uint8_t { kLtpc, kOptionGreeks, kFull, kFullD30 };
enum class Plan : std::uint8_t { kStandard, kPlus };

struct ModeLimit {
  std::size_t individual;  // when only this mode is subscribed
  std::size_t combined;    // per mode, when several modes are subscribed together
};

// Limits per user. The docs don't say whether subscription limits are per connection, so they are
// enforced across all of a user's connections (the stricter reading); one connection is used.
struct Limits {
  std::size_t connections;
  ModeLimit ltpc;
  ModeLimit option_greeks;
  ModeLimit full;
  ModeLimit full_d30;  // {0, 0} = not available on the plan
};

[[nodiscard]] constexpr Limits limits_for(Plan plan) noexcept {
  // Standard: 2 connections; LTPC 5000/2000, Option Greeks 3000/2000, Full 2000/1500.
  // Plus: 5 connections; Full D30 50/1500 (the Plus table lists only D30; others as Standard).
  return plan == Plan::kPlus ? Limits{5, {5000, 2000}, {3000, 2000}, {2000, 1500}, {50, 1500}}
                             : Limits{2, {5000, 2000}, {3000, 2000}, {2000, 1500}, {0, 0}};
}

struct Subscription {
  std::string key;  // instrument_key, e.g. "NSE_FO|50001"
  Mode mode{Mode::kLtpc};
};

// Validates against the plan's limits (F-MD-2 AC1) and returns the deduplicated set (a key in
// several modes keeps the richest; full_d30 > full > option_greeks > ltpc).
[[nodiscard]] std::expected<std::vector<Subscription>, Error> plan_subscriptions(
    std::span<const Subscription> subs, Plan plan);

// Request frames (sent as WebSocket **binary** frames): one "sub" per mode, chunked.
//   {"guid":"vyuha-sub-1","method":"sub","data":{"mode":"full","instrumentKeys":["NSE_FO|50001"]}}
[[nodiscard]] std::vector<std::string> subscribe_messages(std::span<const Subscription> subs,
                                                          std::size_t chunk = 500);

}  // namespace vyuha::broker::upstox

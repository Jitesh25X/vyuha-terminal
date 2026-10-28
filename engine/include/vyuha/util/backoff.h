#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

#include "vyuha/core/clock.h"

namespace vyuha {

// Reconnect delays for market-data sockets (F-MD-2 AC2): exponential from `initial`, doubling up
// to `cap`, with "equal jitter" (uniform in [d/2, d]) so many shards don't reconnect in lockstep.
struct BackoffPolicy {
  Nanos initial{500'000'000};
  Nanos cap{30 * kNanosPerSecond};
};

class Backoff {
 public:
  explicit Backoff(BackoffPolicy policy = {}, std::uint64_t seed = std::random_device{}())
      : policy_(policy), rng_(seed) {}

  // Delay before the next attempt; grows with each call until reset().
  [[nodiscard]] Nanos next() noexcept {
    const Nanos ceiling =
        attempts_ >= kMaxShift ? policy_.cap : std::min(policy_.cap, policy_.initial << attempts_);
    ++attempts_;
    std::uniform_int_distribution<Nanos> jitter(ceiling / 2, ceiling);
    return jitter(rng_);
  }
  // After a connection proved healthy.
  void reset() noexcept { attempts_ = 0; }
  [[nodiscard]] std::uint32_t attempts() const noexcept { return attempts_; }

 private:
  static constexpr std::uint32_t kMaxShift = 32;  // initial << 32 would overflow long before cap matters
  BackoffPolicy policy_;
  std::mt19937_64 rng_;
  std::uint32_t attempts_{0};
};

}  // namespace vyuha

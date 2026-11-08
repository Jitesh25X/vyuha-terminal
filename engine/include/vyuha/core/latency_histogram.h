#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "vyuha/core/clock.h"

namespace vyuha {

// A cumulative latency histogram with fixed bounds, in the shape of a Prometheus histogram: no allocation,
// so the core thread can record into it. Readers take a copy on the same thread (the exporter arrives with
// the engine's metrics endpoint).
class LatencyHistogram {
 public:
  // Upper bounds, in nanoseconds; the last bucket is +Inf.
  static constexpr std::array<Nanos, 12> kBounds{
      10'000,     50'000,     100'000,    500'000,    1'000'000,   5'000'000,
      10'000'000, 25'000'000, 50'000'000, 75'000'000, 100'000'000, 250'000'000,
  };
  static constexpr std::size_t kBuckets = kBounds.size() + 1;

  void record(Nanos value) noexcept {
    const auto* const it = std::ranges::lower_bound(kBounds, value);
    // At most kBounds.size(), the +Inf bucket: always in range.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    ++counts_[static_cast<std::size_t>(it - kBounds.begin())];
    ++count_;
    sum_ += value;
    max_ = std::max(max_, value);
  }

  // Observations in bucket i alone (not cumulative): bounds[i-1] < v <= bounds[i].
  [[nodiscard]] std::uint64_t bucket(std::size_t i) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): checked
    return i < kBuckets ? counts_[i] : 0;
  }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] Nanos sum() const noexcept { return sum_; }
  [[nodiscard]] Nanos max() const noexcept { return max_; }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t count_{0};
  Nanos sum_{0};
  Nanos max_{0};
};

}  // namespace vyuha

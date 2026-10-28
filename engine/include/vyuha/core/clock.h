#pragma once

#include <atomic>
#include <cstdint>

namespace vyuha {

// Nanoseconds since the Unix epoch, UTC (CLAUDE.md invariant 9: store UTC, display IST).
using Nanos = std::int64_t;

inline constexpr Nanos kNanosPerSecond = 1'000'000'000;
inline constexpr Nanos kNanosPerMinute = 60 * kNanosPerSecond;

// Injected time source: WallClock live, SimClock in tests and backtests. Engine code never reads
// the system clock directly (scripts/check-invariants.sh).
class IClock {
 public:
  IClock() = default;
  IClock(const IClock&) = delete;
  IClock& operator=(const IClock&) = delete;
  IClock(IClock&&) = delete;
  IClock& operator=(IClock&&) = delete;
  virtual ~IClock() = default;
  [[nodiscard]] virtual Nanos now() const noexcept = 0;
};

class WallClock final : public IClock {
 public:
  [[nodiscard]] Nanos now() const noexcept override;
};

// Manually advanced clock. Readable from any thread; advanced by the test/backtest driver.
class SimClock final : public IClock {
 public:
  explicit SimClock(Nanos start = 0) noexcept : now_(start) {}
  [[nodiscard]] Nanos now() const noexcept override { return now_.load(std::memory_order_acquire); }
  void set(Nanos t) noexcept { now_.store(t, std::memory_order_release); }
  void advance(Nanos d) noexcept { now_.fetch_add(d, std::memory_order_acq_rel); }

 private:
  std::atomic<Nanos> now_;
};

}  // namespace vyuha

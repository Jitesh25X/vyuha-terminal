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
  // Nanoseconds since an arbitrary start. Never steps, and keeps counting while the host is suspended (D-34),
  // so every duration a decision rests on uses it (D-25, D-26). Comparable only with other readings of this
  // clock, so never publish or store it.
  [[nodiscard]] virtual Nanos monotonic() const noexcept = 0;
};

class WallClock final : public IClock {
 public:
  [[nodiscard]] Nanos now() const noexcept override;
  [[nodiscard]] Nanos monotonic() const noexcept override;
};

// Manually advanced clock. Readable from any thread; advanced by the test/backtest driver.
class SimClock final : public IClock {
 public:
  explicit SimClock(Nanos start = 0) noexcept : now_(start), monotonic_(start) {}
  [[nodiscard]] Nanos now() const noexcept override { return now_.load(std::memory_order_acquire); }
  [[nodiscard]] Nanos monotonic() const noexcept override {
    return monotonic_.load(std::memory_order_acquire);
  }
  // Time passing moves both clocks. Going backwards can only be a wall-clock step, so monotonic stays put.
  void set(Nanos t) noexcept { elapse(t - now_.exchange(t, std::memory_order_acq_rel)); }
  void advance(Nanos d) noexcept {
    now_.fetch_add(d, std::memory_order_acq_rel);
    elapse(d);
  }
  // The wall clock alone jumps, as an NTP correction does.
  void step_wall(Nanos d) noexcept { now_.fetch_add(d, std::memory_order_acq_rel); }
  // The host was suspended for d: both clocks count it (D-34). A monotonic clock that stops during suspend
  // would move the wall clock alone, which is what step_wall(d) stands for.
  void suspend(Nanos d) noexcept { advance(d); }

 private:
  void elapse(Nanos d) noexcept {
    if (d > 0) {
      monotonic_.fetch_add(d, std::memory_order_acq_rel);
    }
  }

  std::atomic<Nanos> now_;
  std::atomic<Nanos> monotonic_;
};

}  // namespace vyuha

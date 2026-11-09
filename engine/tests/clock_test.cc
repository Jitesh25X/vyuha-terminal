// The injected clocks (D-25, D-26, D-34).

#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime is POSIX, and <ctime> needn't declare it

#include <gtest/gtest.h>

#include "vyuha/core/clock.h"
#include "vyuha/core/monotonic_source.h"

namespace {

using vyuha::kNanosPerSecond;
using vyuha::Nanos;
using vyuha::SimClock;
using vyuha::WallClock;

// The clock WallClock::monotonic must read: one that counts while the host is suspended. Pinned at compile
// time, because a CI runner that never suspended can't tell the clocks apart at run time.
#if defined(__linux__)
static_assert(vyuha::kMonotonicClockId == CLOCK_BOOTTIME);
#else
static_assert(vyuha::kMonotonicClockId == CLOCK_MONOTONIC);
#endif
constexpr clockid_t kCountsSuspend = vyuha::kMonotonicClockId;

Nanos read(clockid_t id) {
  timespec ts{};
  EXPECT_EQ(clock_gettime(id, &ts), 0);
  return (static_cast<Nanos>(ts.tv_sec) * kNanosPerSecond) + static_cast<Nanos>(ts.tv_nsec);
}

// A real suspend can't be made in a test, so this checks the reading against the clock the header names. It
// tells that clock from steady_clock only where the two differ: on a Mac by NTP slew (about 5.7 s over two
// days), on a Linux host that has been suspended since boot by the time suspended. A CI runner that never
// suspended passes either way, which is why the id itself is pinned by the static_assert above.
TEST(WallClock, MonotonicReadsTheClockThatCountsSuspend) {
  const WallClock clock;
  const Nanos before = read(kCountsSuspend);
  const Nanos got = clock.monotonic();
  const Nanos after = read(kCountsSuspend);
  EXPECT_GE(got, before);
  EXPECT_LE(got, after);
}

TEST(WallClock, MonotonicIsPositiveAndNeverGoesBack) {
  const WallClock clock;
  Nanos last = clock.monotonic();
  EXPECT_GT(last, 0);
  for (int i = 0; i < 10'000; ++i) {
    const Nanos next = clock.monotonic();
    ASSERT_GE(next, last);
    last = next;
  }
}

TEST(SimClock, ASuspendMovesBothClocks) {
  SimClock clock{1'000};
  clock.suspend(10 * kNanosPerSecond);
  EXPECT_EQ(clock.now(), 1'000 + (10 * kNanosPerSecond));
  EXPECT_EQ(clock.monotonic(), 1'000 + (10 * kNanosPerSecond));
}

TEST(SimClock, AWallStepMovesOnlyTheWallClock) {
  SimClock clock{1'000};
  clock.step_wall(10 * kNanosPerSecond);
  EXPECT_EQ(clock.now(), 1'000 + (10 * kNanosPerSecond));
  EXPECT_EQ(clock.monotonic(), 1'000);
  clock.step_wall(-20 * kNanosPerSecond);
  EXPECT_EQ(clock.monotonic(), 1'000);
}

}  // namespace

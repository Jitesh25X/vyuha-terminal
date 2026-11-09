// The injected clocks (D-25, D-26, D-34).

#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime is POSIX, and <ctime> needn't declare it

#include <gtest/gtest.h>

#include "vyuha/core/clock.h"
#include "vyuha/core/monotonic_source.h"
#include "vyuha/core/pause_watch.h"
#include "vyuha/core/slew_check.h"

namespace {

using vyuha::check_chrony_slew;
using vyuha::kChronyDefaultSlewPpm;
using vyuha::kNanosPerSecond;
using vyuha::kPauseGap;
using vyuha::Nanos;
using vyuha::PauseWatch;
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

// D-36. A pause is the wall clock moving more than kPauseGap past the monotonic one between two looks.
TEST(PauseWatch, TheFirstLookOnlyRecords) {
  PauseWatch watch;
  EXPECT_FALSE(watch.observe(1'000'000'000'000, 5));
}

TEST(PauseWatch, BothClocksMovingTogetherIsNoPause) {
  PauseWatch watch;
  EXPECT_FALSE(watch.observe(1'000'000'000'000, 5));
  EXPECT_FALSE(watch.observe(1'000'000'000'000 + (3'600 * kNanosPerSecond), 5 + (3'600 * kNanosPerSecond)));
}

TEST(PauseWatch, TheGapItselfIsNotAPauseButOneNanosecondMoreIs) {
  PauseWatch at_gap;
  EXPECT_FALSE(at_gap.observe(1'000, 0));
  EXPECT_FALSE(at_gap.observe(1'000 + kPauseGap, 0));
  PauseWatch past_gap;
  EXPECT_FALSE(past_gap.observe(1'000, 0));
  EXPECT_TRUE(past_gap.observe(1'000 + kPauseGap + 1, 0));
}

TEST(PauseWatch, EachLookIsMeasuredFromTheLastOne) {
  PauseWatch watch;
  EXPECT_FALSE(watch.observe(1'000, 0));
  EXPECT_TRUE(watch.observe(1'000 + kPauseGap + 1, 0));
  EXPECT_FALSE(watch.observe(1'000 + kPauseGap + 1 + kNanosPerSecond, kNanosPerSecond));  // nothing new
}

TEST(PauseWatch, AWallClockStepBackIsNeverAPause) {
  PauseWatch watch;
  EXPECT_FALSE(watch.observe(1'000'000'000'000, 0));
  EXPECT_FALSE(watch.observe(1'000'000'000'000 - (10 * kNanosPerSecond), 0));
}

TEST(PauseWatch, AClockThatCountedASuspendIsNotAPause) {
  SimClock clock{1'000'000'000'000};
  PauseWatch watch;
  EXPECT_FALSE(watch.observe(clock));
  clock.suspend(10 * kNanosPerSecond);
  EXPECT_FALSE(watch.observe(clock));
  clock.step_wall(kPauseGap + 1);
  EXPECT_TRUE(watch.observe(clock));
}

// D-36: the host's NTP slew limit. chrony's default is far above what the OPS window can take.
TEST(SlewCheck, FiveHundredPpmIsFine) {
  const auto check = check_chrony_slew("pool ntp.ubuntu.com iburst\nmaxslewrate 500\n");
  EXPECT_TRUE(check.ok);
  EXPECT_TRUE(check.configured);
  EXPECT_EQ(check.ppm, 500.0);
}

TEST(SlewCheck, AnythingAboveItIsNot) {
  EXPECT_FALSE(check_chrony_slew("maxslewrate 500.5").ok);
  EXPECT_FALSE(check_chrony_slew("maxslewrate 83333.333").ok);
  EXPECT_TRUE(check_chrony_slew("maxslewrate 83333.333").configured);
}

TEST(SlewCheck, NoSettingMeansChronysDefault) {
  const auto check = check_chrony_slew("pool ntp.ubuntu.com iburst\nmakestep 1 3\n");
  EXPECT_FALSE(check.ok);
  EXPECT_FALSE(check.configured);
  EXPECT_EQ(check.ppm, kChronyDefaultSlewPpm);
  EXPECT_FALSE(check_chrony_slew("").ok);
}

TEST(SlewCheck, CommentsAndIndentationAndWindowsLineEndings) {
  EXPECT_FALSE(check_chrony_slew("# maxslewrate 100\n! maxslewrate 100\n; maxslewrate 100\n").configured);
  EXPECT_TRUE(check_chrony_slew("   \tmaxslewrate   100\r\n").ok);
  EXPECT_TRUE(check_chrony_slew("maxslewrate\t400").ok);
}

TEST(SlewCheck, EveryLineMustBeLowEnough) {
  const auto check = check_chrony_slew("maxslewrate 100\nmaxslewrate 900\n");
  EXPECT_FALSE(check.ok);
  EXPECT_EQ(check.ppm, 900.0);
  EXPECT_FALSE(check_chrony_slew("maxslewrate 900\nmaxslewrate 100\n").ok);
}

TEST(SlewCheck, ADirectiveIsMatchedWithoutRegardToCase) {
  EXPECT_TRUE(check_chrony_slew("MaxSlewRate 100").ok);
  EXPECT_FALSE(check_chrony_slew("maxslewrate 100\nMAXSLEWRATE 83333").ok);
}

TEST(SlewCheck, ZeroOrAStrayArgumentIsUnreadable) {
  EXPECT_FALSE(check_chrony_slew("maxslewrate 0").ok);
  EXPECT_TRUE(check_chrony_slew("maxslewrate 0").unreadable);
  EXPECT_FALSE(check_chrony_slew("maxslewrate 500 ppm").ok);
  EXPECT_TRUE(check_chrony_slew("maxslewrate 500 ppm").unreadable);
  EXPECT_TRUE(check_chrony_slew("maxslewrate 500 # the cap\n").ok);
  EXPECT_FALSE(check_chrony_slew("maxslewrate abc").configured);
}

// D-36: a pause is only seen if chrony steps the clock instead of slewing it away.
TEST(SlewCheck, MakestepWithAnUnlimitedLimitStepsAtOnce) {
  EXPECT_TRUE(check_chrony_slew("makestep 1 -1\n").steps);
  EXPECT_TRUE(check_chrony_slew("MAKESTEP 0.5 -1 # always").steps);
}

TEST(SlewCheck, AFiniteOrMissingOrBadMakestepDoesNot) {
  EXPECT_FALSE(check_chrony_slew("makestep 1 3\n").steps);  // Ubuntu's default: three steps after start
  EXPECT_FALSE(check_chrony_slew("").steps);
  EXPECT_FALSE(check_chrony_slew("makestep 1\n").steps);
  EXPECT_FALSE(check_chrony_slew("makestep 0 -1\n").steps);
  EXPECT_FALSE(check_chrony_slew("makestep 1 -x\n").steps);
  EXPECT_FALSE(check_chrony_slew("makestep 1 -1 extra\n").steps);
  EXPECT_FALSE(check_chrony_slew("makestep 1 -1\nmakestep 1 3\n").steps);  // every line has to
  EXPECT_FALSE(check_chrony_slew("makestep 1 3\nmakestep 1 -1\n").steps);  // (chrony uses the last; stricter)
  EXPECT_FALSE(check_chrony_slew("makestep 1 10\n").steps);
  EXPECT_FALSE(check_chrony_slew("makestep 1 12\n").steps);  // a positive limit has no minus sign
  EXPECT_FALSE(check_chrony_slew("makestep 1 -0\n").steps);  // chrony reads an integer: never step
  EXPECT_FALSE(check_chrony_slew("makestep 1 -0.0\n").steps);
  EXPECT_TRUE(check_chrony_slew("makestep 1 -12\n").steps);
}

TEST(SlewCheck, AValueThatIsNotANumberIsNotOk) {
  EXPECT_FALSE(check_chrony_slew("maxslewrate abc").ok);
  EXPECT_FALSE(check_chrony_slew("maxslewrate").ok);
  EXPECT_FALSE(check_chrony_slew("maxslewrate 5oo").ok);
  EXPECT_FALSE(check_chrony_slew("maxslewrate 100\nmaxslewrate x").ok);
  EXPECT_FALSE(check_chrony_slew("maxslewrate -5").ok);
}

}  // namespace

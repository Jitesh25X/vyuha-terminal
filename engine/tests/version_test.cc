#include "vyuha/core/version.h"

#include <expected>

#include <gtest/gtest.h>

namespace {

TEST(Version, MatchesProject) { EXPECT_EQ(vyuha::version(), "0.0.1"); }

// Guards the toolchain floor: the engine's error model relies on C++23 std::expected.
TEST(Toolchain, HasStdExpected) {
  const std::expected<int, std::string_view> ok = 42;
  const std::expected<int, std::string_view> err = std::unexpected("bad");
  EXPECT_EQ(ok.value(), 42);
  EXPECT_EQ(err.error(), "bad");
}

}  // namespace

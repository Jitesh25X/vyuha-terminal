#pragma once

// A test that needs a service skips when it isn't configured, for quick local runs. With VYUHA_REQUIRE_DB=1
// (CI and `make verify`) it fails instead, so a service being down can never look like a pass.

#include <cstdlib>
#include <string_view>

#include <gtest/gtest.h>

namespace vyuha::testing {

inline bool services_required() {
  const char* v = std::getenv("VYUHA_REQUIRE_DB");  // NOLINT(concurrency-mt-unsafe): test setup
  return v != nullptr && std::string_view(v) == "1";
}

}  // namespace vyuha::testing

// A macro because GTEST_SKIP and GTEST_FAIL return from the test. Use it only in a TEST body or SetUp: from a
// helper, the return leaves the helper and the test goes on. One statement (gtest's own switch guard), so an
// unbraced `if (...) VYUHA_SKIP_WITHOUT_SERVICE(...);` can't leave the skip outside the if.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VYUHA_SKIP_WITHOUT_SERVICE(message)                                     \
  switch (0)                                                                    \
  case 0:                                                                       \
  default:                                                                      \
    if (vyuha::testing::services_required()) {                                  \
      GTEST_FAIL() << (message) << " and VYUHA_REQUIRE_DB=1: it must not skip"; \
    } else                                                                      \
      GTEST_SKIP() << (message)

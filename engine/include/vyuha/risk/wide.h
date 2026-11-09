#pragma once

#include <cstdint>
#include <limits>

namespace vyuha::risk {

// Prices and quantities come from callers, so the products of them can't be allowed to overflow.
__extension__ using Wide = __int128;

[[nodiscard]] constexpr std::int64_t saturate(Wide value) noexcept {
  if (value > std::numeric_limits<std::int64_t>::max()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value < std::numeric_limits<std::int64_t>::min()) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] constexpr Wide wide_abs(Wide value) noexcept { return value < 0 ? -value : value; }

}  // namespace vyuha::risk

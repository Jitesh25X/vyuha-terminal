#pragma once

#include <cstdint>
#include <string_view>

namespace vyuha {

enum class ErrorCode : std::uint8_t {
  kInvalidArgument = 1,
  kOutOfRange,
  kCapacityExceeded,
};

// Errors are values (std::expected<T, Error>); `detail` points at static text, never at user data.
struct Error {
  ErrorCode code;
  std::string_view detail;
};

}  // namespace vyuha

#pragma once

#include <cstdint>
#include <string_view>

namespace vyuha {

enum class ErrorCode : std::uint8_t {
  kInvalidArgument = 1,
  kOutOfRange,
  kCapacityExceeded,
  kStaticIpNotOnHost,  // invariant 4 preflight (STATIC_IP_NOT_ON_HOST)
  kConnectFailed,
};

// Errors are values (std::expected<T, Error>); `detail` points at static text, never at user data.
struct Error {
  ErrorCode code;
  std::string_view detail;
};

}  // namespace vyuha

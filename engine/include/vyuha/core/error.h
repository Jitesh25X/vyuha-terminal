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
  kIllegalTransition,    // OMS_ILLEGAL_TRANSITION (F-EX-2)
  kOrderDup,             // ORDER_DUP: this client_order_id already has an order with a different body
  kOrderRecovery,        // open orders in Postgres couldn't be read back
  kOrderLogUnavailable,  // order events aren't reaching Postgres, so nothing may be sent
};

// Errors are values (std::expected<T, Error>); `detail` points at static text, never at user data.
struct Error {
  ErrorCode code;
  std::string_view detail;
};

}  // namespace vyuha

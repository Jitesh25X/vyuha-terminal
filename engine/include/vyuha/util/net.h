#pragma once

#include <expected>
#include <string_view>

#include "vyuha/core/error.h"

namespace vyuha::net {

// Invariant 4 preflight: the account's configured source IP must be assigned to a local
// interface before any broker socket binds to it. kStaticIpNotOnHost otherwise.
[[nodiscard]] std::expected<void, Error> check_ip_on_host(std::string_view ip);

}  // namespace vyuha::net

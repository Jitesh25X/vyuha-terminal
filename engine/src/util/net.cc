#include "vyuha/util/net.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace vyuha::net {

std::expected<void, Error> check_ip_on_host(std::string_view ip) {
  const std::string text(ip);
  in_addr v4{};
  in6_addr v6{};
  const bool is_v4 = ::inet_pton(AF_INET, text.c_str(), &v4) == 1;
  const bool is_v6 = !is_v4 && ::inet_pton(AF_INET6, text.c_str(), &v6) == 1;
  if (!is_v4 && !is_v6) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "source ip: not an IP address"});
  }
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) {
    return std::unexpected(Error{ErrorCode::kStaticIpNotOnHost, "source ip: cannot list host interfaces"});
  }
  bool found = false;
  for (const ifaddrs* it = list; it != nullptr && !found; it = it->ifa_next) {
    if (it->ifa_addr == nullptr) {
      continue;
    }
    if (is_v4 && it->ifa_addr->sa_family == AF_INET) {
      in_addr a{};
      std::memcpy(&a, &reinterpret_cast<const sockaddr_in*>(it->ifa_addr)->sin_addr, sizeof a);  // NOLINT
      found = a.s_addr == v4.s_addr;
    } else if (is_v6 && it->ifa_addr->sa_family == AF_INET6) {
      in6_addr a{};
      std::memcpy(&a, &reinterpret_cast<const sockaddr_in6*>(it->ifa_addr)->sin6_addr, sizeof a);  // NOLINT
      found = std::memcmp(&a, &v6, sizeof a) == 0;
    }
  }
  ::freeifaddrs(list);
  if (!found) {
    return std::unexpected(Error{ErrorCode::kStaticIpNotOnHost, "STATIC_IP_NOT_ON_HOST"});
  }
  return {};
}

}  // namespace vyuha::net

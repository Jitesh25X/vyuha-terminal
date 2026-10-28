#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "vyuha/broker/feed.h"
#include "vyuha/broker/upstox/feed_parser.h"
#include "vyuha/broker/upstox/subscriptions.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"
#include "vyuha/core/error.h"
#include "vyuha/util/backoff.h"
#include "vyuha/util/secret.h"

namespace vyuha::broker::upstox {

struct FeedConfig {
  // Authorize call: GET <authorize_path> with the bearer token returns a single-use wss:// URL.
  std::string authorize_host{"api.upstox.com"};
  std::string authorize_port{"443"};
  std::string authorize_path{"/v3/feed/market-data-feed/authorize"};
  bool tls{true};  // tests use plain http/ws against local fixture servers
  SecretRef access_token;
  std::string source_ip;  // invariant 4
  Plan plan{Plan::kStandard};
  BackoffPolicy backoff{};
  Nanos idle_timeout{30 * kNanosPerSecond};  // Upstox sends WebSocket pings when idle
  std::string ca_file;
  std::uint64_t jitter_seed{0};
};

struct FeedStats {
  std::uint64_t connects{0};
  std::uint64_t disconnects{0};
  std::uint64_t authorizations{0};
  std::uint64_t frames{0};
  std::uint64_t ticks{0};
  std::uint64_t ring_full{0};
  std::uint64_t market_info{0};
  std::uint64_t unknown_key{0};
  std::uint64_t bad_price{0};
  std::uint64_t malformed{0};
};

// Upstox Market Data Feed V3 client: authorize → WSS from the bound source IP, binary JSON
// subscription requests, protobuf frames decoded without allocation into the core's TickRing,
// reconnect (re-authorizing: the URL is single-use) with backoff, full resubscription, gap log.
class UpstoxFeed {
 public:
  UpstoxFeed(FeedConfig cfg, KeyMap keys, TickRing& out, const IClock& clock);
  UpstoxFeed(const UpstoxFeed&) = delete;
  UpstoxFeed& operator=(const UpstoxFeed&) = delete;
  UpstoxFeed(UpstoxFeed&&) = delete;
  UpstoxFeed& operator=(UpstoxFeed&&) = delete;
  ~UpstoxFeed();

  [[nodiscard]] std::expected<void, Error> start(std::span<const Subscription> subs);
  void stop();
  [[nodiscard]] FeedStats stats() const noexcept;
  [[nodiscard]] std::vector<Gap> gaps() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Splits "wss://host[:port]/path?query" (also ws://). Exposed for tests.
struct WsUrl {
  bool tls{true};
  std::string host;
  std::string port;
  std::string target;
};
[[nodiscard]] std::expected<WsUrl, Error> parse_ws_url(std::string_view url);

}  // namespace vyuha::broker::upstox

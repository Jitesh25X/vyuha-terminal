#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "vyuha/broker/feed.h"
#include "vyuha/broker/kite/ticker_parser.h"
#include "vyuha/broker/kite/ticker_shards.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"
#include "vyuha/core/error.h"
#include "vyuha/util/backoff.h"
#include "vyuha/util/secret.h"

namespace vyuha::broker::kite {

struct TickerConfig {
  std::string host{"ws.kite.trade"};
  std::string port{"443"};
  bool tls{true};  // tests use plain ws:// against a local fixture server
  std::string api_key;
  SecretRef access_token;  // never logged; the URL carrying it is never logged either
  std::string source_ip;   // invariant 4: every socket binds here before connecting
  TickerLimits limits{};
  BackoffPolicy backoff{};
  Nanos idle_timeout{10 * kNanosPerSecond};  // Kite heartbeats every ~2 s when quiet
  std::string ca_file;                       // empty: probe the usual system bundles
  std::uint64_t jitter_seed{0};              // 0: random
};

using broker::Gap;

struct TickerStats {
  std::uint64_t connects{0};
  std::uint64_t disconnects{0};
  std::uint64_t frames{0};
  std::uint64_t ticks{0};
  std::uint64_t ring_full{0};  // core fell behind: tick dropped (the next tick supersedes it)
  std::uint64_t text_frames{0};
  std::uint64_t unknown_token{0};
  std::uint64_t malformed{0};
};

// Kite Ticker WebSocket client: sharded connections (≤ 3 per key, ≤ 3 000 tokens each), binary
// decode on one IO thread into the core's TickRing (its only producer), reconnect with
// exponential backoff + jitter and full resubscription. Zero live calls in tests: they point
// `host`/`port` at a local server replaying fixtures.
class KiteTicker {
 public:
  KiteTicker(TickerConfig cfg, TokenMap tokens, TickRing& out, const IClock& clock);
  KiteTicker(const KiteTicker&) = delete;
  KiteTicker& operator=(const KiteTicker&) = delete;
  KiteTicker(KiteTicker&&) = delete;
  KiteTicker& operator=(KiteTicker&&) = delete;
  ~KiteTicker();

  // Preflight (source IP on host, shard plan within limits), then connects in the background.
  [[nodiscard]] std::expected<void, Error> start(std::span<const Subscription> subs);
  void stop();

  [[nodiscard]] TickerStats stats() const noexcept;
  [[nodiscard]] std::vector<Gap> gaps() const;
  [[nodiscard]] std::size_t shard_count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vyuha::broker::kite

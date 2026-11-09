#pragma once
// Internal to the broker adapters (exposes Boost.Asio). Public APIs are KiteTicker / UpstoxFeed.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "vyuha/broker/feed.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/error.h"
#include "vyuha/util/backoff.h"

namespace vyuha::broker {

// Where a shard connects. Upstox issues a fresh single-use URL per connection, so this is asked
// for again before every attempt.
struct FeedEndpoint {
  std::string host;
  std::string port{"443"};
  std::string target{"/"};
  bool tls{true};
  std::vector<std::pair<std::string, std::string>> headers;  // may carry credentials: never logged
};

// HTTPS (or HTTP for local tests) GET from the account's source IP, for broker handshakes such as
// Upstox's authorize call. Returns the body of a 2xx response.
class HttpGetter {
 public:
  HttpGetter() = default;
  HttpGetter(const HttpGetter&) = delete;
  HttpGetter& operator=(const HttpGetter&) = delete;
  HttpGetter(HttpGetter&&) = delete;
  HttpGetter& operator=(HttpGetter&&) = delete;
  virtual ~HttpGetter() = default;
  virtual boost::asio::awaitable<std::expected<std::string, Error>> get(FeedEndpoint ep) = 0;
};

// Broker-specific half of a market-data feed. Called only on the feed's IO thread.
class FeedProtocol {
 public:
  FeedProtocol() = default;
  FeedProtocol(const FeedProtocol&) = delete;
  FeedProtocol& operator=(const FeedProtocol&) = delete;
  FeedProtocol(FeedProtocol&&) = delete;
  FeedProtocol& operator=(FeedProtocol&&) = delete;
  virtual ~FeedProtocol() = default;

  // `http` is the FeedClient that runs the coroutine, so it outlives it.
  virtual boost::asio::awaitable<std::expected<FeedEndpoint, Error>> endpoint(
      std::size_t shard,
      HttpGetter& http) = 0;  // NOLINT(cppcoreguidelines-avoid-reference-coroutine-parameters)
  // Sent after every (re)connect, so a reconnect restores the full subscription set.
  [[nodiscard]] virtual std::vector<std::string> subscribe_messages(std::size_t shard) const = 0;
  [[nodiscard]] virtual bool binary_requests() const noexcept = 0;
  virtual void on_frame(std::size_t shard, bool binary, std::span<const std::uint8_t> bytes, Nanos now,
                        Nanos recv_mono) = 0;
};

struct FeedClientConfig {
  std::string name;       // for logs, e.g. "kite"
  std::string source_ip;  // invariant 4: every socket binds here before connecting
  BackoffPolicy backoff{};
  Nanos idle_timeout{10 * kNanosPerSecond};
  std::string ca_file;  // empty: probe the usual system bundles
  std::uint64_t jitter_seed{0};
  bool tls{true};  // load a TLS context (tests against local servers turn this off)
};

// Broker-neutral WebSocket feed runner: one IO thread, one reconnect loop per shard (exponential
// backoff with jitter), source-IP binding after a host-interface preflight, TLS with hostname
// verification, idle timeout, gap log.
class FeedClient {
 public:
  FeedClient(FeedClientConfig cfg, FeedProtocol& protocol, const IClock& clock);
  FeedClient(const FeedClient&) = delete;
  FeedClient& operator=(const FeedClient&) = delete;
  FeedClient(FeedClient&&) = delete;
  FeedClient& operator=(FeedClient&&) = delete;
  ~FeedClient();

  [[nodiscard]] std::expected<void, Error> start(std::size_t shards);
  void stop();
  [[nodiscard]] FeedConnStats stats() const noexcept;
  [[nodiscard]] std::vector<Gap> gaps() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vyuha::broker

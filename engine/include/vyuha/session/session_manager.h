#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vyuha/broker/upstox/subscriptions.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"
#include "vyuha/core/error.h"
#include "vyuha/md/tick.h"
#include "vyuha/session/instrument_directory.h"
#include "vyuha/util/backoff.h"
#include "vyuha/util/secret.h"

namespace vyuha::session {

enum class BrokerKind : std::uint8_t { kZerodha, kUpstox, kSim };
enum class FeedMode : std::uint8_t { kLtp, kQuote, kFull };
enum class SessionState : std::uint8_t { kDisconnected, kAwaitingLogin, kConnected, kError };

[[nodiscard]] std::string_view to_string(SessionState s) noexcept;

// One broker account's session (mirrors engine.v1.StartSessionRequest; secrets as SecretRef).
struct SessionRequest {
  std::string account_id;
  BrokerKind broker{BrokerKind::kSim};
  std::string api_key;
  SecretRef api_secret;
  SecretRef access_token;  // daily; empty = awaiting login
  std::string source_ip;   // invariant 4: required unless sim
  std::string backup_source_ip;
  std::string public_ip;
  std::uint32_t ops_cap{8};
  bool data_entitled{false};
  broker::upstox::Plan upstox_plan{broker::upstox::Plan::kStandard};
};

struct SessionStatus {
  std::string account_id;
  std::uint32_t feed_id{0};  // stamped on this session's ticks; unique per start
  BrokerKind broker{BrokerKind::kSim};
  SessionState state{SessionState::kDisconnected};
  std::string detail;  // stable code, e.g. STATIC_IP_NOT_ON_HOST
  std::uint32_t ops_cap{0};
  bool data_entitled{false};
  std::size_t subscribed{0};  // instruments on the running feed
  std::size_t unresolved{0};  // requested ids with no mapping for this broker
  std::uint64_t feed_restarts{0};
  std::uint64_t feed_connects{0};
  std::uint64_t feed_ticks{0};
  std::size_t feed_gaps{0};
};

// Where feeds connect. Production defaults; tests point them at local servers.
struct SessionManagerConfig {
  std::string kite_host{"ws.kite.trade"};
  std::string kite_port{"443"};
  std::string upstox_authorize_host{"api.upstox.com"};
  std::string upstox_authorize_port{"443"};
  bool tls{true};
  BackoffPolicy backoff{};
  // Subscription changes are applied by restarting the account's feed, at most this often.
  Nanos resubscribe_debounce{1 * kNanosPerSecond};
  bool background_apply{true};  // false in tests: call apply_pending() explicitly
};

// Control plane for broker sessions (gRPC StartSession/StopSession/GetSessions/Subscribe).
// Thread-safe; never on the core thread. Each data-entitled session gets one TickRing from the
// pool (its feed's IO thread is that ring's only producer).
class SessionManager {
 public:
  SessionManager(SessionManagerConfig cfg, const InstrumentDirectory& instruments,
                 std::span<TickRing* const> rings, const IClock& clock);
  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
  SessionManager(SessionManager&&) = delete;
  SessionManager& operator=(SessionManager&&) = delete;
  ~SessionManager();

  // Preflight (source IP on a host interface) then registers the session. A failed preflight is
  // kept as an `error` session with detail STATIC_IP_NOT_ON_HOST and logged as an admin alert.
  [[nodiscard]] std::expected<SessionStatus, Error> start(SessionRequest req);
  [[nodiscard]] std::expected<void, Error> stop(std::string_view account_id);
  [[nodiscard]] std::vector<SessionStatus> sessions() const;

  // Market-data interest per requester ("gateway", "strategy:<id>"). `via_account` empty = the
  // first connected data-entitled session. Effective mode per instrument = max over requesters.
  [[nodiscard]] std::expected<void, Error> subscribe(std::string_view requester,
                                                     std::span<const md::InstrumentId> ids, FeedMode mode,
                                                     std::string_view via_account);
  [[nodiscard]] std::expected<void, Error> unsubscribe(std::string_view requester,
                                                       std::span<const md::InstrumentId> ids,
                                                       std::string_view via_account);

  // Restarts feeds whose subscriptions changed and whose debounce elapsed (also run in background).
  void apply_pending();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vyuha::session

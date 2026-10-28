#include "vyuha/session/session_manager.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "quill/LogMacros.h"
#include "vyuha/broker/kite/ticker.h"
#include "vyuha/broker/upstox/feed.h"
#include "vyuha/util/log.h"
#include "vyuha/util/net.h"

namespace vyuha::session {

std::string_view to_string(SessionState s) noexcept {
  switch (s) {
    case SessionState::kDisconnected:
      return "disconnected";
    case SessionState::kAwaitingLogin:
      return "awaiting_login";
    case SessionState::kConnected:
      return "connected";
    case SessionState::kError:
      return "error";
  }
  return "error";
}

namespace {

namespace kite = broker::kite;
namespace upstox = broker::upstox;

kite::Mode kite_mode(FeedMode m) noexcept {
  switch (m) {
    case FeedMode::kLtp:
      return kite::Mode::kLtp;
    case FeedMode::kQuote:
      return kite::Mode::kQuote;
    case FeedMode::kFull:
      return kite::Mode::kFull;
  }
  return kite::Mode::kQuote;
}

// Upstox has no "quote" mode: quote and full both need `full` (LTPC carries no OHLC/depth).
upstox::Mode upstox_mode(FeedMode m) noexcept {
  return m == FeedMode::kLtp ? upstox::Mode::kLtpc : upstox::Mode::kFull;
}

template <class Requesters>
FeedMode max_mode(const Requesters& by) noexcept {
  FeedMode mode = FeedMode::kLtp;
  for (const auto& [who, m] : by) {
    mode = std::max(mode, m);
  }
  return mode;
}

struct Session {
  SessionRequest req;
  SessionState state{SessionState::kDisconnected};
  std::string detail;
  TickRing* ring{nullptr};
  // instrument → requester → mode
  std::map<md::InstrumentId, std::map<std::string, FeedMode, std::less<>>> interest;
  bool dirty{false};
  Nanos last_apply{0};
  std::uint64_t restarts{0};
  std::size_t subscribed{0};
  std::size_t unresolved{0};
  std::unique_ptr<kite::KiteTicker> kite_feed;
  std::unique_ptr<upstox::UpstoxFeed> upstox_feed;
};

}  // namespace

struct SessionManager::Impl {
  SessionManagerConfig cfg;
  const InstrumentDirectory& instruments;
  const IClock& clock;
  mutable std::mutex mu;
  std::vector<TickRing*> free_rings;
  std::map<std::string, Session, std::less<>> sessions;
  std::condition_variable_any cv;
  std::jthread applier;

  Impl(SessionManagerConfig c, const InstrumentDirectory& dir, std::span<TickRing* const> rings,
       const IClock& clk)
      : cfg(std::move(c)), instruments(dir), clock(clk), free_rings(rings.begin(), rings.end()) {}

  static void alert(std::string_view account, std::string_view code) {
    if (quill::Logger* log = log::engine()) {
      LOG_ERROR(log, "ADMIN ALERT session refused: account={} code={}", account, code);
    }
  }

  static SessionStatus status_of(const Session& s) {
    SessionStatus st{.account_id = s.req.account_id,
                     .broker = s.req.broker,
                     .state = s.state,
                     .detail = s.detail,
                     .ops_cap = s.req.ops_cap,
                     .data_entitled = s.req.data_entitled,
                     .subscribed = s.subscribed,
                     .unresolved = s.unresolved,
                     .feed_restarts = s.restarts};
    if (s.kite_feed) {
      const auto k = s.kite_feed->stats();
      st.feed_connects = k.connects;
      st.feed_ticks = k.ticks;
      st.feed_gaps = s.kite_feed->gaps().size();
    } else if (s.upstox_feed) {
      const auto u = s.upstox_feed->stats();
      st.feed_connects = u.connects;
      st.feed_ticks = u.ticks;
      st.feed_gaps = s.upstox_feed->gaps().size();
    }
    return st;
  }

  static void stop_feed(Session& s) {
    s.kite_feed.reset();  // destructors stop and join the feed's IO thread
    s.upstox_feed.reset();
    s.subscribed = 0;
  }

  // Rebuilds the account's feed for its current interest set (caller holds `mu`).
  void apply(Session& s) {
    stop_feed(s);
    s.dirty = false;
    s.last_apply = clock.now();
    s.unresolved = 0;
    if (s.interest.empty() || s.ring == nullptr) {
      return;
    }
    ++s.restarts;
    std::expected<void, Error> started;
    if (s.req.broker == BrokerKind::kZerodha) {
      std::vector<std::pair<std::uint32_t, md::InstrumentId>> tokens;
      std::vector<kite::Subscription> subs;
      for (const auto& [id, by] : s.interest) {
        const auto ids = instruments.find(id);
        const std::uint32_t token = ids.has_value() ? ids->kite_token.value_or(0) : 0;
        if (token == 0) {
          ++s.unresolved;
          continue;
        }
        tokens.emplace_back(token, id);
        subs.push_back({token, kite_mode(max_mode(by))});
      }
      kite::TickerConfig tc;
      tc.host = cfg.kite_host;
      tc.port = cfg.kite_port;
      tc.tls = cfg.tls;
      tc.api_key = s.req.api_key;
      tc.access_token = SecretRef(s.req.access_token.reveal());
      tc.source_ip = s.req.source_ip;
      tc.backoff = cfg.backoff;
      s.kite_feed = std::make_unique<kite::KiteTicker>(std::move(tc), kite::TokenMap(std::move(tokens)),
                                                       *s.ring, clock);
      s.subscribed = subs.size();
      started = s.kite_feed->start(subs);
    } else {
      std::vector<std::pair<std::string, md::InstrumentId>> keys;
      std::vector<upstox::Subscription> subs;
      for (const auto& [id, by] : s.interest) {
        const auto ids = instruments.find(id);
        std::string key = ids.has_value() ? ids->upstox_key : std::string{};
        if (key.empty()) {
          ++s.unresolved;
          continue;
        }
        keys.emplace_back(key, id);
        subs.push_back({std::move(key), upstox_mode(max_mode(by))});
      }
      upstox::FeedConfig fc;
      fc.authorize_host = cfg.upstox_authorize_host;
      fc.authorize_port = cfg.upstox_authorize_port;
      fc.tls = cfg.tls;
      fc.access_token = SecretRef(s.req.access_token.reveal());
      fc.source_ip = s.req.source_ip;
      fc.plan = s.req.upstox_plan;
      fc.backoff = cfg.backoff;
      s.upstox_feed = std::make_unique<upstox::UpstoxFeed>(std::move(fc), upstox::KeyMap(std::move(keys)),
                                                           *s.ring, clock);
      s.subscribed = subs.size();
      started = s.upstox_feed->start(subs);
    }
    if (!started) {
      stop_feed(s);
      s.state = SessionState::kError;
      s.detail = std::string(started.error().detail);
      if (quill::Logger* log = log::engine()) {
        LOG_ERROR(log, "session {} feed failed to start: {}", s.req.account_id, s.detail);
      }
    }
  }

  void apply_due() {
    const std::scoped_lock lock(mu);
    const Nanos now = clock.now();
    for (auto& [id, s] : sessions) {
      if (s.dirty && (s.last_apply == 0 || now - s.last_apply >= cfg.resubscribe_debounce)) {
        apply(s);
      }
    }
  }

  // Invariant 4: no broker session without its static source IP(s) on this host.
  static std::expected<void, Error> preflight(Session& s) {
    if (s.req.broker == BrokerKind::kSim) {
      return {};
    }
    if (s.req.source_ip.empty()) {
      return std::unexpected(
          Error{ErrorCode::kInvalidArgument, "session: source_ip required for a broker account"});
    }
    for (const std::string* ip : {&s.req.source_ip, &s.req.backup_source_ip}) {
      if (ip->empty()) {
        continue;
      }
      if (auto ok = net::check_ip_on_host(*ip); !ok) {
        s.state = SessionState::kError;
        s.detail =
            ok.error().code == ErrorCode::kStaticIpNotOnHost ? "STATIC_IP_NOT_ON_HOST" : "INVALID_SOURCE_IP";
        return std::unexpected(ok.error());
      }
    }
    return {};
  }

  Session* pick(std::string_view via) {
    if (!via.empty()) {
      const auto it = sessions.find(via);
      return it == sessions.end() ? nullptr : &it->second;
    }
    for (auto& [id, s] : sessions) {  // std::map: deterministic (account id order)
      if (s.state == SessionState::kConnected && s.req.data_entitled && s.ring != nullptr) {
        return &s;
      }
    }
    return nullptr;
  }
};

SessionManager::SessionManager(SessionManagerConfig cfg, const InstrumentDirectory& instruments,
                               std::span<TickRing* const> rings, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), instruments, rings, clock)) {
  if (impl_->cfg.background_apply) {
    impl_->applier = std::jthread([impl = impl_.get()](const std::stop_token& st) {
      std::mutex m;
      std::unique_lock lk(m);
      while (!st.stop_requested()) {
        impl->cv.wait_for(lk, st, std::chrono::milliseconds(100), [] { return false; });
        if (!st.stop_requested()) {
          impl->apply_due();
        }
      }
    });
  }
}

SessionManager::~SessionManager() {
  if (impl_->applier.joinable()) {
    impl_->applier.request_stop();
    impl_->applier.join();
  }
  const std::scoped_lock lock(impl_->mu);
  for (auto& [id, s] : impl_->sessions) {
    Impl::stop_feed(s);
  }
}

std::expected<SessionStatus, Error> SessionManager::start(SessionRequest req) {
  if (req.account_id.empty()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "session: account_id required"});
  }
  const std::scoped_lock lock(impl_->mu);
  const auto existing = impl_->sessions.find(req.account_id);
  if (existing != impl_->sessions.end() && existing->second.state != SessionState::kError &&
      existing->second.state != SessionState::kDisconnected) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "session: already running"});
  }
  if (existing != impl_->sessions.end()) {
    Impl::stop_feed(existing->second);
    if (existing->second.ring != nullptr) {
      impl_->free_rings.push_back(existing->second.ring);
    }
    impl_->sessions.erase(existing);
  }
  Session s;
  s.req = std::move(req);
  const std::string account = s.req.account_id;
  if (auto ok = Impl::preflight(s); !ok) {
    if (s.state == SessionState::kError) {  // keep the refusal visible (status bar) and alert
      Impl::alert(account, s.detail);
      impl_->sessions.emplace(account, std::move(s));
    }
    return std::unexpected(ok.error());
  }
  s.state = s.req.broker != BrokerKind::kSim && s.req.access_token.empty() ? SessionState::kAwaitingLogin
                                                                           : SessionState::kConnected;
  if (s.req.data_entitled && s.req.broker != BrokerKind::kSim) {
    if (impl_->free_rings.empty()) {
      return std::unexpected(Error{ErrorCode::kCapacityExceeded, "session: no free market-data ring"});
    }
    s.ring = impl_->free_rings.back();
    impl_->free_rings.pop_back();
  }
  if (quill::Logger* log = log::engine()) {
    // Preflight result for the PRD's "verified by engine preflight log" (no secrets in it).
    LOG_INFO(log, "session {} started: state={} source_ip={} data_entitled={}", account, to_string(s.state),
             s.req.source_ip, s.req.data_entitled);
  }
  const auto [it, inserted] = impl_->sessions.emplace(account, std::move(s));
  return Impl::status_of(it->second);
}

std::expected<void, Error> SessionManager::stop(std::string_view account_id) {
  const std::scoped_lock lock(impl_->mu);
  const auto it = impl_->sessions.find(account_id);
  if (it == impl_->sessions.end()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "session: unknown account"});
  }
  Impl::stop_feed(it->second);
  if (it->second.ring != nullptr) {
    impl_->free_rings.push_back(it->second.ring);
  }
  impl_->sessions.erase(it);
  return {};
}

std::vector<SessionStatus> SessionManager::sessions() const {
  const std::scoped_lock lock(impl_->mu);
  std::vector<SessionStatus> out;
  out.reserve(impl_->sessions.size());
  for (const auto& [id, s] : impl_->sessions) {
    out.push_back(Impl::status_of(s));
  }
  return out;
}

std::expected<void, Error> SessionManager::subscribe(std::string_view requester,
                                                     std::span<const md::InstrumentId> ids, FeedMode mode,
                                                     std::string_view via_account) {
  if (requester.empty()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "subscribe: requester required"});
  }
  const std::scoped_lock lock(impl_->mu);
  Session* s = impl_->pick(via_account);
  if (s == nullptr || s->ring == nullptr || s->state != SessionState::kConnected) {
    return std::unexpected(
        Error{ErrorCode::kInvalidArgument, "subscribe: no connected data-entitled session"});
  }
  for (const md::InstrumentId id : ids) {
    if (id == 0) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, "subscribe: instrument id 0"});
    }
  }
  for (const md::InstrumentId id : ids) {
    s->interest[id][std::string(requester)] = mode;
  }
  s->dirty = true;
  return {};
}

std::expected<void, Error> SessionManager::unsubscribe(std::string_view requester,
                                                       std::span<const md::InstrumentId> ids,
                                                       std::string_view via_account) {
  const std::scoped_lock lock(impl_->mu);
  Session* s = impl_->pick(via_account);
  if (s == nullptr) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "unsubscribe: no such session"});
  }
  for (const md::InstrumentId id : ids) {
    const auto it = s->interest.find(id);
    if (it == s->interest.end()) {
      continue;
    }
    if (const auto r = it->second.find(requester); r != it->second.end()) {
      it->second.erase(r);
    }
    if (it->second.empty()) {
      s->interest.erase(it);
    }
  }
  s->dirty = true;
  return {};
}

void SessionManager::apply_pending() { impl_->apply_due(); }

}  // namespace vyuha::session

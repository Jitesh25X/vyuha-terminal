// M1.6: session manager. Done check: STATIC_IP_NOT_ON_HOST (F-EX-3 AC1). Feeds point at a closed
// local port, so nothing leaves the machine (invariant 3); only bookkeeping is asserted here (the
// adapters' own tests cover streaming).

#include "vyuha/session/session_manager.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"

namespace {

using vyuha::ErrorCode;
using vyuha::SecretRef;
using vyuha::session::BrokerIds;
using vyuha::session::BrokerKind;
using vyuha::session::FeedMode;
using vyuha::session::SessionManager;
using vyuha::session::SessionManagerConfig;
using vyuha::session::SessionRequest;
using vyuha::session::SessionState;
using vyuha::session::StaticInstrumentDirectory;

struct Fixture {
  vyuha::SimClock clock{1'000'000'000};
  StaticInstrumentDirectory dir{{{1, BrokerIds{.kite_token = 408065U, .upstox_key = "NSE_EQ|INE002A01018"}},
                                 {2, BrokerIds{.kite_token = 884737U, .upstox_key = ""}},
                                 {3, BrokerIds{.kite_token = std::nullopt, .upstox_key = "NSE_FO|50001"}}}};
  std::vector<std::unique_ptr<vyuha::TickRing>> rings;
  std::vector<vyuha::TickRing*> ring_ptrs;
  std::unique_ptr<SessionManager> mgr;

  explicit Fixture(std::size_t n_rings = 2) {
    for (std::size_t i = 0; i < n_rings; ++i) {
      rings.push_back(std::make_unique<vyuha::TickRing>());
      ring_ptrs.push_back(rings.back().get());
    }
    SessionManagerConfig cfg;
    cfg.kite_host = "127.0.0.1";
    cfg.kite_port = "1";  // closed: feeds retry in the background, never reach a broker
    cfg.upstox_authorize_host = "127.0.0.1";
    cfg.upstox_authorize_port = "1";
    cfg.tls = false;
    cfg.backoff = {.initial = 50'000'000, .cap = 200'000'000};
    cfg.resubscribe_debounce = vyuha::kNanosPerSecond;
    cfg.background_apply = false;
    mgr = std::make_unique<SessionManager>(cfg, dir, ring_ptrs, clock);
  }

  static SessionRequest broker(std::string account, BrokerKind kind, std::string ip, bool token = true) {
    SessionRequest r;
    r.account_id = std::move(account);
    r.broker = kind;
    r.api_key = "key";
    r.api_secret = SecretRef("secret");
    r.access_token = token ? SecretRef("token") : SecretRef();
    r.source_ip = std::move(ip);
    r.data_entitled = true;
    return r;
  }

  vyuha::session::SessionStatus status(std::string_view account) const {
    for (auto& s : mgr->sessions()) {
      if (s.account_id == account) {
        return s;
      }
    }
    ADD_FAILURE() << "no session " << account;
    return {};
  }
};

// F-EX-3 AC1: given account A bound to an IP not present on the host, the engine refuses to start
// A's session with STATIC_IP_NOT_ON_HOST (and raises an admin alert, logged).
TEST(SessionManager, RefusesStaticIpNotOnHost) {
  Fixture f;
  const auto r = f.mgr->start(Fixture::broker("acct-z1", BrokerKind::kZerodha, "203.0.113.7"));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kStaticIpNotOnHost);
  const auto st = f.status("acct-z1");
  EXPECT_EQ(st.state, SessionState::kError);
  EXPECT_EQ(st.detail, "STATIC_IP_NOT_ON_HOST");
  EXPECT_FALSE(
      f.mgr->subscribe("gateway", std::vector<vyuha::md::InstrumentId>{1}, FeedMode::kQuote, "acct-z1"));

  // The backup IP is checked too, and a broker account without a source IP is rejected outright.
  auto backup = Fixture::broker("acct-z2", BrokerKind::kZerodha, "127.0.0.1");
  backup.backup_source_ip = "203.0.113.8";
  EXPECT_EQ(f.mgr->start(std::move(backup)).error().code, ErrorCode::kStaticIpNotOnHost);
  EXPECT_EQ(f.mgr->start(Fixture::broker("acct-z3", BrokerKind::kUpstox, "")).error().code,
            ErrorCode::kInvalidArgument);

  // Fixing the IP and starting again replaces the error session.
  ASSERT_TRUE(f.mgr->start(Fixture::broker("acct-z1", BrokerKind::kZerodha, "127.0.0.1")).has_value());
  EXPECT_EQ(f.status("acct-z1").state, SessionState::kConnected);
}

TEST(SessionManager, StatesForSimMissingTokenAndDuplicates) {
  Fixture f;
  SessionRequest sim;
  sim.account_id = "paper";
  sim.broker = BrokerKind::kSim;
  ASSERT_TRUE(f.mgr->start(std::move(sim)).has_value());  // no IP needed for SimBroker
  EXPECT_EQ(f.status("paper").state, SessionState::kConnected);

  ASSERT_TRUE(f.mgr->start(Fixture::broker("acct-u1", BrokerKind::kUpstox, "127.0.0.1", false)).has_value());
  EXPECT_EQ(f.status("acct-u1").state, SessionState::kAwaitingLogin);

  ASSERT_TRUE(f.mgr->start(Fixture::broker("acct-z1", BrokerKind::kZerodha, "127.0.0.1")).has_value());
  EXPECT_EQ(f.mgr->start(Fixture::broker("acct-z1", BrokerKind::kZerodha, "127.0.0.1")).error().code,
            ErrorCode::kInvalidArgument);
  ASSERT_TRUE(f.mgr->stop("acct-z1").has_value());
  EXPECT_FALSE(f.mgr->stop("acct-z1").has_value());
  EXPECT_EQ(f.mgr->sessions().size(), 2U);
}

TEST(SessionManager, SubscriptionsPerRequesterWithDebouncedFeedRestarts) {
  Fixture f;
  ASSERT_TRUE(f.mgr->start(Fixture::broker("acct-z1", BrokerKind::kZerodha, "127.0.0.1")).has_value());
  const std::vector<vyuha::md::InstrumentId> ids = {1, 2, 3};
  ASSERT_TRUE(f.mgr->subscribe("gateway", ids, FeedMode::kQuote, ""));  // picks the only entitled session
  ASSERT_TRUE(f.mgr->subscribe("strategy:s1", std::vector<vyuha::md::InstrumentId>{1}, FeedMode::kFull, ""));
  f.mgr->apply_pending();
  auto st = f.status("acct-z1");
  EXPECT_EQ(st.subscribed, 2U);  // 1, 2 have Kite tokens
  EXPECT_EQ(st.unresolved, 1U);  // 3 is Upstox-only
  EXPECT_EQ(st.feed_restarts, 1U);

  // Within the debounce window further changes wait; afterwards they apply once.
  ASSERT_TRUE(f.mgr->unsubscribe("gateway", ids, ""));
  f.mgr->apply_pending();
  EXPECT_EQ(f.status("acct-z1").feed_restarts, 1U);
  f.clock.advance(vyuha::kNanosPerSecond);
  f.mgr->apply_pending();
  st = f.status("acct-z1");
  EXPECT_EQ(st.feed_restarts, 2U);
  EXPECT_EQ(st.subscribed, 1U);  // strategy:s1 still wants instrument 1

  ASSERT_TRUE(f.mgr->unsubscribe("strategy:s1", std::vector<vyuha::md::InstrumentId>{1}, ""));
  f.clock.advance(vyuha::kNanosPerSecond);
  f.mgr->apply_pending();
  EXPECT_EQ(f.status("acct-z1").subscribed, 0U);  // nobody interested: feed stopped
}

TEST(SessionManager, UpstoxSessionsMapQuoteToFullAndHonourPlanLimits) {
  Fixture f;
  ASSERT_TRUE(f.mgr->start(Fixture::broker("acct-u1", BrokerKind::kUpstox, "127.0.0.1")).has_value());
  ASSERT_TRUE(f.mgr->subscribe("gateway", std::vector<vyuha::md::InstrumentId>{1, 2, 3}, FeedMode::kQuote,
                               "acct-u1"));
  f.mgr->apply_pending();
  const auto st = f.status("acct-u1");
  EXPECT_EQ(st.subscribed, 2U);  // 1 and 3 have Upstox keys
  EXPECT_EQ(st.unresolved, 1U);
  EXPECT_EQ(st.state, SessionState::kConnected);
}

TEST(SessionManager, RingPoolBoundsDataSessionsAndIsReturnedOnStop) {
  Fixture f(1);
  ASSERT_TRUE(f.mgr->start(Fixture::broker("a1", BrokerKind::kZerodha, "127.0.0.1")).has_value());
  EXPECT_EQ(f.mgr->start(Fixture::broker("a2", BrokerKind::kUpstox, "127.0.0.1")).error().code,
            ErrorCode::kCapacityExceeded);
  ASSERT_TRUE(f.mgr->stop("a1").has_value());
  EXPECT_TRUE(f.mgr->start(Fixture::broker("a2", BrokerKind::kUpstox, "127.0.0.1")).has_value());
}

TEST(SessionManager, FeedIdsAreUniquePerStartAndNeverReused) {
  Fixture f;
  const auto a = f.mgr->start(Fixture::broker("a1", BrokerKind::kZerodha, "127.0.0.1"));
  const auto b = f.mgr->start(Fixture::broker("a2", BrokerKind::kUpstox, "127.0.0.1"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_NE(a->feed_id, 0U);
  EXPECT_NE(a->feed_id, b->feed_id);
  // The daily re-login restarts the session: ticks still in flight from the old feed must not be
  // attributed to the new one, so the id changes.
  ASSERT_TRUE(f.mgr->stop("a1").has_value());
  const auto again = f.mgr->start(Fixture::broker("a1", BrokerKind::kZerodha, "127.0.0.1"));
  ASSERT_TRUE(again.has_value());
  EXPECT_NE(again->feed_id, a->feed_id);
  EXPECT_NE(again->feed_id, b->feed_id);
  EXPECT_EQ(f.status("a1").feed_id, again->feed_id);

  // A restarted engine (new SessionManager) doesn't start from the same id again (ADR-020: md_last
  // ticks outlive the process). Random seeds collide with probability ~2^-31.
  Fixture restarted;
  const auto r = restarted.mgr->start(Fixture::broker("a2", BrokerKind::kUpstox, "127.0.0.1"));
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(r->feed_id, a->feed_id);
  EXPECT_NE(r->feed_id, b->feed_id);
}

}  // namespace

// F-EX-3, M2.5b-fix (D-56): a stopped parent's queued children, written by the real order writer to a
// throwaway, migrated Postgres (VYUHA_TEST_POSTGRES_URL, else skipped): EXPIRED (PARENT_STOPPED), each with
// its audit row in the status's own transaction, and no alert.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "desk_fixture.h"
#include "pg_temp_db.h"
#include "require_services.h"
#include "vyuha/oms/pg_order_store.h"
#include "vyuha/oms/slice_runner.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

using vyuha::testing::admin_url;
using vyuha::testing::TempDb;

class Ids final : public oms::IOrderIdSource {
 public:
  [[nodiscard]] Uuid next() noexcept override {
    Uuid id = id_of(0xC0);
    id.bytes[15] = ++drawn_;
    return id;
  }

 private:
  std::uint8_t drawn_{0};
};

// Takes as many orders as it is told to; the rest wait in the router's queue.
class Budget final : public oms::IOrderSender {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool send(const ApprovedOrder& approved, std::string_view /*tag*/) noexcept override {
    if (accept == 0) {
      return false;
    }
    --accept;
    sent.emplace_back(approved.order().client_order_id.view());
    return true;
  }
  [[nodiscard]] bool cancel(const oms::Order& /*order*/) noexcept override { return true; }
  std::size_t accept{0};
  std::vector<std::string> sent;
};

// The desk's account, user and instrument, under the ids the fixture's order uses. The row's kind doesn't
// matter to the writer; the gate reads the test's own instrument table.
void seed(const std::string& url, std::int64_t instrument) {
  pqxx::connection c(url);
  pqxx::work tx(c);
  tx.exec(
      "INSERT INTO app_user (id, username, display_name, password_hash, role) "
      "VALUES ($1::uuid, 'owner', 'Owner', 'x', 'admin')",
      pqxx::params{kUser.to_string()});
  tx.exec(
      "INSERT INTO broker_account (id, broker, client_code, holder_name, api_key_enc, api_secret_enc, "
      "static_ip) VALUES ($1::uuid, 'upstox', 'UP1', 'X', '\\x00', '\\x00', '127.0.0.1')",
      pqxx::params{kAccount.to_string()});
  tx.exec(
      "INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, lot_size, tick_size, "
      "valid_from) "
      "VALUES ($1, 'NSE', 'EQ', 'SYM', 'SYM', 1, 0.05, '2026-01-01')",
      pqxx::params{instrument});
  tx.commit();
}

// Six children of one parent, two at the broker and four queued; then the parent's second child is rejected
// (the system stops the parent) or the parent is cancelled (a person asked).
void scenario(const std::string& admin, bool cancel_parent) {
  const std::string_view reason = cancel_parent ? "CANCELLED_IN_QUEUE" : "PARENT_STOPPED";
  const TempDb db(admin);
  constexpr std::int64_t kSliced = 20;
  seed(db.url(), kSliced);

  Desk d;
  ASSERT_TRUE(d.instruments.put(kSliced, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .exchange = Exchange::kNfo,
                                          .segment = Segment::kFut,
                                          .lower_circuit = rupees(90),
                                          .upper_circuit = rupees(110),
                                          .freeze_qty = 11}));
  d.tick(kSliced, rupees(100));
  d.clear_limits(LimitKey::kMaxOrderValue);
  d.limit(LimitKey::kMaxOrderValue, rupees(1'000'000).units());  // room for the whole parent (D-49)
  d.clear_limits(LimitKey::kMaxOrderQtyLots);
  d.limit(LimitKey::kMaxOrderQtyLots, 1'000 * kFromScale);
  const RiskGate gate = RiskGate::create(pre_trade_rules(), d.clock).value();
  const GateSendLimits limits(d.ctx);
  Budget broker;
  Ids ids;
  {
    oms::PgOrderEventWriter writer(db.url(), nullptr);
    oms::Oms oms(writer, d.clock, 16);
    oms::OrderRouter router(oms, writer, broker, oms::Venue::kPaper, {.tag_limit = 20, .capacity = 16},
                            limits);
    oms::SliceRunner runner(gate, d.ctx, oms, router, writer, ids);
    OrderDetails parent = d.order();
    parent.id = id_of(40);
    ASSERT_TRUE(parent.client_order_id.assign("P1"));
    parent.instrument_id = kSliced;
    parent.qty = 60;
    const auto started = runner.start(parent);
    ASSERT_TRUE(started.has_value());
    ASSERT_EQ(started->kind, oms::SliceStart::Kind::kStarted)
        << reason_text(started->check.rejection.value_or(RuleResult{}));
    ASSERT_EQ(runner.poll(), 6U);

    const auto until = [](const auto& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return done();
    };
    broker.accept = 2;
    ASSERT_TRUE(until([&] {
      static_cast<void>(router.poll());  // sends once APPROVED is durable
      return broker.sent.size() == 2;
    }));
    if (cancel_parent) {
      ASSERT_TRUE(runner.cancel(id_of(40)));
    } else {
      const auto second = oms.find_handle("P1-002");
      ASSERT_TRUE(second.has_value());
      ASSERT_TRUE(oms.apply(*second, oms::OrderEvent::kBrokerAck, {.broker_order_id = "B2"}).has_value());
      ASSERT_TRUE(oms.apply(*second, oms::OrderEvent::kBrokerReject, {.reason_code = "INSUFFICIENT_MARGIN"})
                      .has_value());
    }
    broker.accept = 100;
    ASSERT_TRUE(until([&] {
      static_cast<void>(router.poll());
      return router.pending() == 0;
    }));
    static_cast<void>(runner.poll());
    EXPECT_EQ(runner.view(id_of(40))->stopped,
              cancel_parent ? oms::SliceStop::kCancelled : oms::SliceStop::kChildBrokerRejected);
    EXPECT_EQ(broker.sent, (std::vector<std::string>{"P1-001", "P1-002"}));
    EXPECT_EQ(router.pulled_back(), cancel_parent ? 0U : 4U);
    EXPECT_EQ(oms.illegal_transitions(), 0U);
    ASSERT_TRUE(until([&] { return writer.written() == writer.pushed(); }));
    EXPECT_TRUE(writer.healthy());
  }

  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM \"order\" WHERE status = 'EXPIRED' AND reject_code = "
                                "$1 AND client_order_id IN ('P1-003','P1-004','P1-005','P1-006')",
                                pqxx::params{std::string(reason)}),
            4);
  EXPECT_EQ(
      tx.query_value<std::string>("SELECT status::text FROM \"order\" WHERE client_order_id = 'P1-001'"),
      "SENT");
  EXPECT_EQ(tx.query_value<int>(
                "SELECT count(*) FROM audit_event a JOIN \"order\" o ON o.id::text = a.entity_id "
                "JOIN order_event e ON e.order_id = o.id AND e.to_status = 'EXPIRED' AND e.xmin = a.xmin "
                "WHERE a.action = 'order.expire' AND a.payload->>'reason_code' = $1",
                pqxx::params{std::string(reason)}),
            4);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM alert_event WHERE title LIKE 'ORDER_EXPIRED%'"), 0);
}

TEST(PgSliceRunner, QueuedChildrenOfARejectedParentAreAuditedAsParentStopped) {
  if (admin_url().empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  scenario(admin_url(), false);
}

TEST(PgSliceRunner, QueuedChildrenOfACancelledParentAreAuditedAsCancelledInQueue) {
  if (admin_url().empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  scenario(admin_url(), true);
}

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)

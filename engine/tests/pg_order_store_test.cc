// The order_event writer and recovery against a fresh, migrated Postgres (VYUHA_TEST_POSTGRES_URL, else
// skipped).

#include "vyuha/oms/pg_order_store.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <memory>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "pg_temp_db.h"
#include "require_services.h"
#include "test_gate.h"
#include "vyuha/core/clock.h"
#include "vyuha/oms/oms.h"
#include "vyuha/oms/order_router.h"

namespace vyuha::oms {
namespace {

using vyuha::testing::admin_url;
using vyuha::testing::TempDb;
using enum OrderStatus;
using E = OrderEvent;

class TickingClock final : public IClock {
 public:
  [[nodiscard]] Nanos now() const noexcept override { return 1'767'000'000'000'000'000; }
  [[nodiscard]] Nanos monotonic() const noexcept override { return now(); }
};

constexpr Nanos kMonotonic = 1'767'000'000'000'000'000;

struct Seeded {
  std::string account;
  std::string user;
  std::int64_t instrument{0};
};

Seeded seed(const std::string& url) {
  pqxx::connection c(url);
  pqxx::work tx(c);
  Seeded s;
  s.user = tx.query_value<std::string>(
      "INSERT INTO app_user (username, display_name, password_hash, role) "
      "VALUES ('owner', 'Owner', 'x', 'admin') RETURNING id::text");
  s.account = tx.query_value<std::string>(
      "INSERT INTO broker_account (broker, client_code, holder_name, api_key_enc, api_secret_enc, static_ip) "
      "VALUES ('upstox', 'UP1', 'X', '\\x00', '\\x00', '127.0.0.1') RETURNING id::text");
  s.instrument = tx.query_value<std::int64_t>(
      "INSERT INTO instrument (exchange, segment, symbol, tradingsymbol, lot_size, tick_size, valid_from) "
      "VALUES ('NSE', 'EQ', 'SYM', 'SYM', 1, 0.05, '2026-01-01') RETURNING id");
  tx.commit();
  return s;
}

Uuid uuid_of(const std::string& text) {
  const auto id = Uuid::parse(text);
  if (!id.has_value()) {
    ADD_FAILURE() << "bad uuid " << text;
    return {};
  }
  return *id;
}

OrderDetails details(const Seeded& s, int n, std::int32_t qty) {
  OrderDetails d{.is_paper = kPaperOrder};
  d.id = uuid_of("0190a3b4-7c2d-7e11-8f00-0000000000" + std::string(n < 10 ? "0" : "") + std::to_string(n));
  EXPECT_TRUE(d.client_order_id.assign("client-" + std::to_string(n)));
  d.account_id = uuid_of(s.account);
  d.user_id = uuid_of(s.user);
  d.instrument_id = s.instrument;
  d.side = Side::kSell;
  d.qty = qty;
  d.order_type = OrderType::kSl;
  d.product = Product::kNrml;
  d.validity = Validity::kIoc;
  d.source = Source::kStrategy;
  d.price = Price::from_units(24'500'500);
  d.trigger_price = Price::from_units(24'600'000);
  d.market_protection_millipct = 2'500;
  EXPECT_TRUE(d.tag.assign("manual"));
  d.is_paper = kPaperOrder;
  d.created_at = 1'767'000'000'123'456'000;  // to the microsecond: Postgres keeps no more
  return d;
}

void wait_written(const PgOrderEventWriter& writer, std::uint64_t n) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (writer.written() < n && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(writer.written(), n);
}

TEST(PgOrderStore, EveryTransitionIsWrittenAndOpenOrdersComeBack) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  OrderHandle open_handle = 0;
  OrderHandle unknown_handle = 0;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 8);
    open_handle = *oms.create(details(s, 1, 10));
    ASSERT_TRUE(oms.apply(open_handle, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(open_handle, E::kSend));
    ASSERT_TRUE(oms.apply(open_handle, E::kBrokerAck, {.broker_order_id = "B1"}));
    ASSERT_TRUE(oms.apply(open_handle, E::kPartialFill,
                          {.fill_qty = 4, .fill_price = Price::from_units(24'500'000)}));

    const OrderHandle rejected = *oms.create(details(s, 2, 1));
    ASSERT_TRUE(oms.apply(rejected, E::kRiskReject, {.reason_code = "R-KILL"}));

    const OrderHandle filled = *oms.create(details(s, 3, 1));
    ASSERT_TRUE(oms.apply(filled, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(filled, E::kSend));
    ASSERT_TRUE(oms.apply(filled, E::kBrokerAck, {.broker_order_id = "B3"}));
    ASSERT_TRUE(oms.apply(filled, E::kFill, {.fill_qty = 1, .fill_price = Price::from_units(24'500'000)}));

    unknown_handle = *oms.create(details(s, 4, 2));
    ASSERT_TRUE(oms.apply(unknown_handle, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(unknown_handle, E::kSend));
    ASSERT_TRUE(oms.apply(unknown_handle, E::kAckTimeout));
    wait_written(writer, 16);
    EXPECT_TRUE(writer.healthy());
  }

  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM order_event"), 16);
  // seq is gapless and each event starts where the last one ended.
  EXPECT_EQ(
      tx.query_value<int>(
          "SELECT count(*) FROM (SELECT seq, from_status, lag(to_status) OVER w AS prev, "
          "row_number() OVER w AS n FROM order_event WINDOW w AS (PARTITION BY order_id ORDER BY seq)) x "
          "WHERE seq <> n OR from_status IS DISTINCT FROM prev"),
      0);
  const auto row =
      tx.exec(
            "SELECT status::text, filled_qty, broker_order_id, price::text, trigger_price::text, "
            "market_protection_pct::text, reject_code FROM \"order\" WHERE client_order_id = 'client-1'")
          .one_row();
  EXPECT_EQ(row[0].as<std::string>(), "PARTIALLY_FILLED");
  EXPECT_EQ(row[1].as<int>(), 4);
  EXPECT_EQ(row[2].as<std::string>(), "B1");
  EXPECT_EQ(row[3].as<std::string>(), "2450.0500");
  EXPECT_EQ(row[4].as<std::string>(), "2460.0000");
  EXPECT_EQ(row[5].as<std::string>(), "2.500");
  EXPECT_EQ(
      tx.query_value<std::string>("SELECT reject_code FROM \"order\" WHERE client_order_id = 'client-2'"),
      "R-KILL");
  EXPECT_EQ(tx.query_value<std::string>("SELECT payload->>'fill_price' FROM order_event "
                                        "WHERE to_status = 'PARTIALLY_FILLED'"),
            "2450.0000");

  const auto loaded = load_open_orders(db.url());
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 2U);  // terminal orders stay out
  const RecoveredOrder& open = (*loaded)[0];
  EXPECT_EQ(open.status, kPartiallyFilled);
  EXPECT_EQ(open.seq, 5);
  EXPECT_EQ(open.filled_qty, 4);
  EXPECT_EQ(open.broker_order_id, "B1");
  const OrderDetails want = details(s, 1, 10);
  EXPECT_EQ(open.details.id, want.id);
  EXPECT_EQ(open.details.client_order_id.view(), "client-1");
  EXPECT_EQ(open.details.account_id, want.account_id);
  EXPECT_EQ(open.details.instrument_id, s.instrument);
  EXPECT_EQ(open.details.side, Side::kSell);
  EXPECT_EQ(open.details.order_type, OrderType::kSl);
  EXPECT_EQ(open.details.product, Product::kNrml);
  EXPECT_EQ(open.details.validity, Validity::kIoc);
  EXPECT_EQ(open.details.source, Source::kStrategy);
  EXPECT_EQ(open.details.price, want.price);
  EXPECT_EQ(open.details.trigger_price, want.trigger_price);
  EXPECT_EQ(open.details.market_protection_millipct, want.market_protection_millipct);
  EXPECT_EQ(open.details.created_at, want.created_at);
  EXPECT_EQ((*loaded)[1].status, kUnknown);

  // A restarted engine carries on from the stored seq.
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 8);
    const auto h = oms.restore(open);
    ASSERT_TRUE(h.has_value());
    const auto h_unknown = oms.restore((*loaded)[1]);
    ASSERT_TRUE(h_unknown.has_value());
    EXPECT_EQ(oms.apply(*h_unknown, E::kSend).error().code, ErrorCode::kIllegalTransition);
    ASSERT_TRUE(oms.apply(*h, E::kFill, {.fill_qty = 6, .fill_price = Price::from_units(24'500'000)}));
    wait_written(writer, 1);
  }
  pqxx::connection c2(db.url());
  pqxx::read_transaction tx2(c2);
  EXPECT_EQ(tx2.query_value<int>("SELECT max(seq) FROM order_event e JOIN \"order\" o ON o.id = e.order_id "
                                 "WHERE o.client_order_id = 'client-1'"),
            6);
  EXPECT_EQ(tx2.query_value<int>("SELECT filled_qty FROM \"order\" WHERE client_order_id = 'client-1'"), 10);
  const auto after = load_open_orders(db.url());
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->size(), 1U);
}

TEST(PgOrderStore, ADisagreementBetweenOrderAndItsEventsIsRefused) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 1);
    const OrderHandle h = *oms.create(details(s, 1, 1));
    ASSERT_TRUE(oms.apply(h, E::kRiskApprove));
    wait_written(writer, 2);
  }
  {
    pqxx::connection c(db.url());
    pqxx::work tx(c);
    tx.exec("UPDATE \"order\" SET status = 'SENT'");
    tx.commit();
  }
  const auto loaded = load_open_orders(db.url());
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code, ErrorCode::kOrderRecovery);
}

TEST(PgOrderStore, ARetryAfterAnUnknownCommitChangesNothing) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const OrderDetails d = details(s, 1, 1);
  OrderEventRecord first;
  first.details = d;
  first.order_id = d.id;
  first.seq = 1;
  first.ts = d.created_at;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    ASSERT_TRUE(writer.try_push(first));
    ASSERT_TRUE(writer.try_push(first));  // as a retried batch would
    wait_written(writer, 2);
    EXPECT_TRUE(writer.healthy());
  }
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM order_event"), 1);
}

TEST(PgOrderStore, EventsWaitOutAnOutageAndArriveInOrder) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  pqxx::connection control(admin);
  const auto allow = [&](bool on) {
    pqxx::nontransaction(control).exec("ALTER DATABASE " + db.name() + " ALLOW_CONNECTIONS " +
                                       (on ? "true" : "false"));
  };
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 1);
    allow(false);
    const OrderHandle h = *oms.create(details(s, 1, 2));
    ASSERT_TRUE(oms.apply(h, E::kRiskApprove));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (writer.healthy() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_FALSE(writer.healthy());
    EXPECT_EQ(writer.written(), 0U);
    EXPECT_EQ(oms.apply(h, E::kSend).error().code, ErrorCode::kOrderLogUnavailable);  // down: nothing is sent
    // ADR-027: nothing that adds risk is taken in either; it's counted for the alert written on recovery.
    EXPECT_EQ(oms.create(details(s, 2, 2)).error().code, ErrorCode::kOrderLogUnavailable);
    allow(true);
    wait_written(writer, 2);
    while (!writer.healthy() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(writer.healthy());
    EXPECT_EQ(oms.apply(h, E::kSend), kSent);
    wait_written(writer, 3);
  }
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM order_event"), 3);
  EXPECT_EQ(tx.query_value<std::string>("SELECT status::text FROM \"order\""),
            "SENT");  // the refused one isn't there
  const auto outage =
      tx.exec(
            "SELECT severity, (body->>'refused_new_orders')::int, (body->>'duration_ms')::bigint "
            "FROM alert_event WHERE title = 'ORDER_LOG_OUTAGE'")
          .one_row();  // exactly one alert for the outage
  EXPECT_EQ(outage[0].as<std::string>(), "critical");
  EXPECT_EQ(outage[1].as<int>(), 1);
  EXPECT_GT(outage[2].as<std::int64_t>(), 0);
  EXPECT_EQ(tx.query_value<std::int64_t>(
                "SELECT (extract(epoch FROM created_at) * 1000000)::bigint FROM \"order\""),
            1'767'000'000'123'456);
}

// M2.2c: every kind of refusal reaches the recovery alert, and the desk signal fires once each way.
struct RecordingSignal final : IOutageSignal {
  void outage_started() noexcept override { started.fetch_add(1); }
  void outage_ended() noexcept override { ended.fetch_add(1); }
  std::atomic<int> started{0};
  std::atomic<int> ended{0};
};

TEST(PgOrderStore, TheOutageAlertCountsEveryKindOfRefusal) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  pqxx::connection control(admin);
  const auto allow = [&](bool on) {
    pqxx::nontransaction(control).exec("ALTER DATABASE " + db.name() + " ALLOW_CONNECTIONS " +
                                       (on ? "true" : "false"));
  };
  RecordingSignal signal;
  {
    PgOrderEventWriter writer(db.url(), &signal);
    Oms oms(writer, clock, 2);
    const OrderHandle open = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(oms.apply(open, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(open, E::kSend));
    ASSERT_TRUE(oms.apply(open, E::kBrokerAck, {.broker_order_id = "B1"}));
    const OrderHandle other = oms.create(details(s, 2, 10)).value();
    wait_written(writer, 5);
    EXPECT_EQ(signal.started.load(), 0);

    allow(false);
    ASSERT_TRUE(oms.apply(other, E::kRiskApprove));  // the write the writer finds it can't make
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (writer.healthy() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_FALSE(writer.healthy());

    const Price moved = Price::from_units(25'000'000);
    const auto refused = [&](const EventDetails& e) {
      EXPECT_EQ(oms.apply(open, E::kModifyRequest, e).error().code, ErrorCode::kOrderLogUnavailable);
    };
    refused({.modified_qty = 11});
    refused({.modified_qty = 12});
    refused({.modified_price = moved});
    refused({.modified_trigger = moved});
    refused({.modified_qty = 5, .modified_price = moved});
    EXPECT_EQ(oms.create(details(s, 3, 2)).error().code, ErrorCode::kOrderLogUnavailable);

    allow(true);
    wait_written(writer, 6);
    while (!writer.healthy() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(writer.healthy());
  }
  EXPECT_EQ(signal.started.load(), 1);
  EXPECT_EQ(signal.ended.load(), 1);
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  const auto alert =
      tx.exec(
            "SELECT (body->>'refused_new_orders')::int, (body->>'refused_qty_raises')::int, "
            "(body->>'refused_price_modifies')::int FROM alert_event WHERE title = 'ORDER_LOG_OUTAGE'")
          .one_row();
  EXPECT_EQ(alert[0].as<int>(), 1);
  EXPECT_EQ(alert[1].as<int>(), 2);
  EXPECT_EQ(alert[2].as<int>(), 3);
}

// M2.2c: Postgres takes the events again but refuses the alert. With no further events, the idle writer
// retries it until it lands, and writes it exactly once.
TEST(PgOrderStore, AnOutageAlertThatFailsIsRetriedWhileIdleAndLandsOnce) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  pqxx::connection control(admin);
  const auto allow = [&](bool on) {
    pqxx::nontransaction(control).exec("ALTER DATABASE " + db.name() + " ALLOW_CONNECTIONS " +
                                       (on ? "true" : "false"));
  };
  const auto sql = [&](const std::string& statement) {
    pqxx::connection c(db.url());
    pqxx::nontransaction(c).exec(statement);
  };
  const auto alerts = [&] {
    pqxx::connection c(db.url());
    return pqxx::read_transaction(c).query_value<int>(
        "SELECT count(*) FROM alert_event WHERE title = 'ORDER_LOG_OUTAGE'");
  };
  sql(R"(CREATE FUNCTION refuse_outage_alert() RETURNS trigger LANGUAGE plpgsql AS $$
         BEGIN
           IF NEW.title = 'ORDER_LOG_OUTAGE' THEN RAISE EXCEPTION 'refused by the test'; END IF;
           RETURN NEW;
         END $$)");
  sql("CREATE TRIGGER refuse BEFORE INSERT ON alert_event FOR EACH ROW EXECUTE FUNCTION "
      "refuse_outage_alert()");

  PgOrderEventWriter writer(db.url(), nullptr);
  Oms oms(writer, clock, 1);
  allow(false);
  ASSERT_TRUE(oms.create(details(s, 1, 2)));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (writer.healthy() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_FALSE(writer.healthy());
  allow(true);
  wait_written(writer, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(alerts(), 0);  // it was tried and refused

  sql("DROP TRIGGER refuse ON alert_event");
  while (alerts() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(alerts(), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1'300));  // past another retry interval
  EXPECT_EQ(alerts(), 1);
  EXPECT_EQ(writer.written(), 1U);  // the retry owed nothing to a new event
}

TEST(PgOrderStore, ALateFillIsStoredAndAlertsAdminsOnce) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 1);
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(oms.apply(h, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(h, E::kSend));
    ASSERT_TRUE(oms.apply(h, E::kBrokerAck, {.broker_order_id = "B1"}));
    ASSERT_TRUE(oms.apply(h, E::kBrokerCancel, {.reason_code = "EXCHANGE_CANCEL"}));
    ASSERT_TRUE(oms.apply(h, E::kPartialFill, {.fill_qty = 2, .fill_price = Price::from_units(24'500'000)}));
    wait_written(writer, 6);
  }
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<std::string>("SELECT status::text FROM \"order\""), "CANCELLED");
  EXPECT_EQ(tx.query_value<int>("SELECT filled_qty FROM \"order\""), 2);
  const auto row = tx.exec(
                         "SELECT from_status::text, to_status::text, reason_code, payload->>'fill_qty', "
                         "payload->>'fill_price' FROM order_event WHERE seq = 6")
                       .one_row();
  EXPECT_EQ(row[0].as<std::string>(), "CANCELLED");
  EXPECT_EQ(row[1].as<std::string>(), "CANCELLED");
  EXPECT_EQ(row[2].as<std::string>(), "RECON_MISMATCH");
  EXPECT_EQ(row[3].as<std::string>(), "2");
  EXPECT_EQ(row[4].as<std::string>(), "2450.0000");
  const auto alert = tx.exec("SELECT severity, title, body->>'fill_qty' FROM alert_event").one_row();
  EXPECT_EQ(alert[0].as<std::string>(), "critical");
  EXPECT_EQ(alert[1].as<std::string>(), "RECON_MISMATCH");
  EXPECT_EQ(alert[2].as<std::string>(), "2");
}

// The restart rule: the order, its event, the audit row and the owner's alert are one transaction.
TEST(PgOrderStore, AnExpiredOrderIsAuditedAlertedOnceAndStaysOutOfRecovery) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 1);
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(oms.apply(h, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(h, E::kExpire, {.reason_code = kExpiredOnRestart}));
    // A late fill: a RECON_MISMATCH of its own, and not a second expiry.
    ASSERT_TRUE(oms.apply(h, E::kPartialFill, {.fill_qty = 2, .fill_price = Price::from_units(24'500'000)}));
    wait_written(writer, 4);
  }
  pqxx::connection c(db.url());
  {
    pqxx::read_transaction tx(c);
    const auto order = tx.exec("SELECT status::text, reject_code, filled_qty FROM \"order\"").one_row();
    EXPECT_EQ(order[0].as<std::string>(), "EXPIRED");
    EXPECT_EQ(order[1].as<std::string>(), "EXPIRED_ON_RESTART");  // the fill's reason doesn't replace it
    EXPECT_EQ(order[2].as<int>(), 2);

    const auto audit = tx.exec(
                             "SELECT actor_kind, action, entity, entity_id, payload->>'client_order_id', "
                             "payload->>'reason_code' FROM audit_event WHERE action = 'order.expire'")
                           .one_row();
    EXPECT_EQ(audit[0].as<std::string>(), "engine");
    EXPECT_EQ(audit[2].as<std::string>(), "order");
    EXPECT_EQ(audit[3].as<std::string>(), details(s, 1, 10).id.to_string());
    EXPECT_EQ(audit[4].as<std::string>(), "client-1");
    EXPECT_EQ(audit[5].as<std::string>(), "EXPIRED_ON_RESTART");

    const auto alert = tx.exec(
                             "SELECT severity, user_id::text, body->>'client_order_id', body->>'qty' "
                             "FROM alert_event WHERE title = 'ORDER_EXPIRED_ON_RESTART'")
                           .one_row();
    EXPECT_EQ(alert[0].as<std::string>(), "critical");
    EXPECT_EQ(alert[1].as<std::string>(), s.user);
    EXPECT_EQ(alert[2].as<std::string>(), "client-1");
    EXPECT_EQ(alert[3].as<std::string>(), "10");
    EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM alert_event WHERE title = 'RECON_MISMATCH'"), 1);
  }
  // It's terminal, so a restart has nothing to restore, and doesn't fail on the new status.
  const auto loaded = load_open_orders(db.url());
  ASSERT_TRUE(loaded.has_value());
  EXPECT_TRUE(loaded->empty());
}

TEST(PgOrderStore, TradeIdsAndModifiesSurviveARestart) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  const Price fill_price = Price::from_units(24'500'000);
  const Price new_price = Price::from_units(25'000'000);
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 1);
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(oms.apply(h, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(h, E::kSend));
    ASSERT_TRUE(oms.apply(h, E::kBrokerAck, {.broker_order_id = "B1"}));
    ASSERT_TRUE(oms.apply(h, E::kModifyRequest, {.modified_qty = 20, .modified_price = new_price}));
    // Past the old qty before the confirm: the row's CHECK (filled_qty <= qty) must let this in.
    ASSERT_TRUE(
        oms.apply(h, E::kPartialFill, {.fill_qty = 12, .fill_price = fill_price, .broker_trade_id = "T\"1"}));
    ASSERT_TRUE(
        oms.apply(h, E::kPartialFill, {.fill_qty = 12, .fill_price = fill_price, .broker_trade_id = "T\"1"}));
    ASSERT_TRUE(oms.apply(h, E::kModifyConfirm));
    wait_written(writer, 8);
    EXPECT_TRUE(writer.healthy());
  }
  {
    pqxx::connection c(db.url());
    pqxx::read_transaction tx(c);
    const auto row = tx.exec("SELECT qty, price::text, filled_qty FROM \"order\"").one_row();
    EXPECT_EQ(row[0].as<int>(), 20);
    EXPECT_EQ(row[1].as<std::string>(), "2500.0000");
    EXPECT_EQ(row[2].as<int>(), 12);
    EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM order_event WHERE reason_code = 'DUPLICATE_FILL'"),
              1);
    EXPECT_EQ(tx.query_value<std::string>("SELECT payload->>'broker_trade_id' FROM order_event "
                                          "WHERE (payload->>'fill_qty')::int = 12"),
              "T\"1");
    EXPECT_EQ(tx.query_value<std::string>("SELECT payload->>'modified_qty' FROM order_event "
                                          "WHERE to_status = 'MODIFY_PENDING'"),
              "20");
  }
  const auto loaded = load_open_orders(db.url());
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 1U);
  const RecoveredOrder& back = loaded->front();
  EXPECT_EQ(back.details.qty, 20);
  EXPECT_EQ(back.details.price, new_price);
  ASSERT_FALSE(back.trade_ids.empty());
  EXPECT_EQ(back.trade_ids.front(), "T\"1");

  PgOrderEventWriter writer(db.url(), nullptr);
  Oms oms(writer, clock, 1);
  const OrderHandle h = oms.restore(back).value();
  ASSERT_TRUE(
      oms.apply(h, E::kPartialFill, {.fill_qty = 12, .fill_price = fill_price, .broker_trade_id = "T\"1"}));
  EXPECT_EQ(oms.order(h).filled_qty, 12);  // the replay after a restart still counts once
  EXPECT_EQ(oms.duplicate_fills(), 1U);
}

// A different event with a seq that's taken must not touch the row: it would make a finished order look live.
TEST(PgOrderStore, ADifferentEventOnATakenSeqIsRefusedAndChangesNothing) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  PgOrderEventWriter writer(db.url(), nullptr);
  Oms oms(writer, clock, 1);
  const OrderHandle h = oms.create(details(s, 1, 10)).value();
  ASSERT_TRUE(oms.apply(h, E::kRiskApprove));
  ASSERT_TRUE(oms.apply(h, E::kExpire, {.reason_code = kExpiredOnRestart}));
  wait_written(writer, 3);

  OrderEventRecord replay;
  replay.order_id = details(s, 1, 10).id;
  replay.seq = 3;
  replay.from = kApproved;
  replay.to = kSent;
  replay.ts = clock.now();
  ASSERT_TRUE(writer.try_push(replay));
  wait_written(writer, 4);
  EXPECT_TRUE(writer.healthy());  // it failed alone

  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<std::string>("SELECT status::text FROM \"order\""), "EXPIRED");
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM order_event"), 3);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM alert_event WHERE title = 'ORDER_EVENT_REFUSED'"), 1);
}

TEST(PgOrderStore, ARetriedExpiryChangesNothing) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  OrderEventRecord event;
  event.order_id = details(s, 1, 10).id;
  {
    const TickingClock clock;
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 1);
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(oms.apply(h, E::kRiskApprove));
    ASSERT_TRUE(oms.apply(h, E::kExpire, {.reason_code = kExpiredOnRestart}));
    wait_written(writer, 3);
    event.seq = oms.order(h).seq;
    event.from = kApproved;
    event.to = kExpired;
    event.ts = clock.now();
    ASSERT_TRUE(event.reason_code.assign(kExpiredOnRestart));
    ASSERT_TRUE(writer.try_push(event));  // as after a commit whose result was lost
    wait_written(writer, 4);
  }
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM audit_event WHERE action = 'order.expire'"), 1);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM alert_event"), 1);
}

class RecordingBroker final : public IOrderSender {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool send(const risk::ApprovedOrder& /*order*/, std::string_view tag) noexcept override {
    sent.emplace_back(tag);
    return true;
  }
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool cancel(const Order& order) noexcept override {
    cancelled.emplace_back(order.details.client_order_id.view());
    return true;
  }
  std::vector<std::string> sent;
  std::vector<std::string> cancelled;
};

// Owner, 2026-11-08: a stale approval ends the order, and the rejection, its audit row and the admins' alert
// land in one transaction.
TEST(PgOrderStore, AStaleApprovalIsRejectedAuditedAndAlertedTogether) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  // The wall clock has stepped: the audited age must still be the monotonic one the decision used.
  class SteppedWall final : public IClock {
   public:
    [[nodiscard]] Nanos now() const noexcept override { return kMonotonic + (10 * kNanosPerSecond); }
    [[nodiscard]] Nanos monotonic() const noexcept override { return kMonotonic; }
  };
  const SteppedWall clock;
  const SimClock gate_clock(clock.monotonic() - kDefaultApprovalMaxAge - 1'000);
  RecordingBroker broker;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 2);
    OrderRouter router(oms, writer, broker, Venue::kPaper, {.tag_limit = 20, .capacity = 8},
                       testing::open_limits());
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    EXPECT_EQ(router.submit(h, testing::approve(oms.order(h).details, gate_clock)).error().code,
              ErrorCode::kApprovalStale);
    wait_written(writer, 2);
  }
  EXPECT_TRUE(broker.sent.empty());
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  const auto order = tx.exec("SELECT status::text, reject_code FROM \"order\"").one_row();
  EXPECT_EQ(order[0].as<std::string>(), "RISK_REJECTED");
  EXPECT_EQ(order[1].as<std::string>(), "R-APPROVAL-STALE");
  const auto audit =
      tx.exec(
            "SELECT payload->>'reason_code', (payload->>'approval_age_ns')::bigint FROM audit_event "
            "WHERE action = 'order.approval_stale'")
          .one_row();
  EXPECT_EQ(audit[0].as<std::string>(), "R-APPROVAL-STALE");
  EXPECT_EQ(audit[1].as<std::int64_t>(), kDefaultApprovalMaxAge + 1'000);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM alert_event WHERE title = 'APPROVAL_STALE'"), 1);
  EXPECT_EQ(
      tx.query_value<std::int64_t>(
          "SELECT (body::jsonb->>'approval_age_ns')::bigint FROM alert_event WHERE title = 'APPROVAL_STALE'"),
      kDefaultApprovalMaxAge + 1'000);
  EXPECT_EQ(tx.query_value<std::string>("SELECT payload->'gate'->>'ts_ns' FROM order_event WHERE seq = 2"),
            std::to_string(gate_clock.now()));
}

// D-24: when R-DUP's table is full the order is refused, and the desk hears about it in the same transaction.
TEST(PgOrderStore, ARefusalBecauseTheDuplicateTableIsFullIsAuditedAndAlerted) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 2);
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(oms.apply(h, OrderEvent::kRiskReject, {.reason_code = kDupTableFull}).has_value());
    wait_written(writer, 2);
  }
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<std::string>("SELECT reject_code FROM \"order\""), "R-DUP-TABLE-FULL");
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM audit_event WHERE action = 'order.dup_table_full'"), 1);
  const auto alert =
      tx.exec("SELECT severity, title FROM alert_event WHERE title = 'DUP_TABLE_FULL'").one_row();
  EXPECT_EQ(alert[0].as<std::string>(), "critical");
}

// D-23: Postgres records a kill order's real source, and reading it back can't grant the exemption.
TEST(PgOrderStore, AKillOrderIsStoredAsSuchAndRestoredWithoutTheExemption) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  const auto gate_ctx = std::make_unique<risk::GateContext>();
  gate_ctx->kill.block(risk::KillSwitch::Scope::kDesk);
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 2);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): .value() throws, which fails the test
    (void)oms.create(gate_ctx->kill.kill_generated(details(s, 1, 10)).value()).value();
    wait_written(writer, 1);
  }
  {
    pqxx::connection c(db.url());
    pqxx::read_transaction tx(c);
    EXPECT_EQ(tx.query_value<std::string>("SELECT source::text FROM \"order\""), "kill_flatten");
  }
  const auto restored = load_open_orders(db.url());
  ASSERT_TRUE(restored.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access): ASSERT_TRUE above
  ASSERT_EQ(restored->size(), 1U);
  EXPECT_FALSE(restored->front().details.source.kill_generated());
  // NOLINTEND(bugprone-unchecked-optional-access)
}

// A cancel that beats the send: audited as an expiry, and no alert, because someone asked for it.
TEST(PgOrderStore, AnOrderCancelledWhileQueuedIsAuditedWithNoAlert) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  RecordingBroker broker;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 2);
    OrderRouter router(oms, writer, broker, Venue::kPaper, {.tag_limit = 20, .capacity = 8},
                       testing::open_limits());
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(router.submit(h, testing::approve(oms.order(h).details, clock)).has_value());
    ASSERT_TRUE(router.cancel(h));
    wait_written(writer, 3);
  }
  EXPECT_TRUE(broker.sent.empty());
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  const auto order = tx.exec("SELECT status::text, reject_code FROM \"order\"").one_row();
  EXPECT_EQ(order[0].as<std::string>(), "EXPIRED");
  EXPECT_EQ(order[1].as<std::string>(), "CANCELLED_IN_QUEUE");
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM audit_event WHERE action = 'order.expire'"), 1);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM alert_event WHERE title LIKE 'ORDER_EXPIRED%'"), 0);
}

// ADR-027: a queued order past the age limit is expired, never sent, and the expiry, its audit row and the
// admins' alert land in one transaction, as the restart rule's do.
TEST(PgOrderStore, AnOrderExpiredInTheQueueIsAuditedAndAlertedTogether) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  SimClock clock(1'767'000'000'000'000'000);
  RecordingBroker broker;
  {
    PgOrderEventWriter writer(db.url(), nullptr);
    Oms oms(writer, clock, 2);
    OrderRouter router(oms, writer, broker, Venue::kPaper, {.tag_limit = 20, .capacity = 8},
                       testing::open_limits());
    const OrderHandle h = oms.create(details(s, 1, 10)).value();
    ASSERT_TRUE(router.submit(h, testing::approve(oms.order(h).details, clock)));
    clock.advance(kDefaultQueueMaxAge + 1);
    wait_written(writer, 2);
    EXPECT_EQ(router.poll(), 0U);
    EXPECT_EQ(oms.order(h).status, kExpired);
    wait_written(writer, 3);
  }
  EXPECT_TRUE(broker.sent.empty());
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  const auto order = tx.exec("SELECT status::text, reject_code FROM \"order\"").one_row();
  EXPECT_EQ(order[0].as<std::string>(), "EXPIRED");
  EXPECT_EQ(order[1].as<std::string>(), "EXPIRED_IN_QUEUE");
  EXPECT_EQ(tx.query_value<std::string>(
                "SELECT payload->>'reason_code' FROM audit_event WHERE action = 'order.expire'"),
            "EXPIRED_IN_QUEUE");
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM alert_event WHERE title = 'ORDER_EXPIRED_IN_QUEUE'"),
            1);
}

// A duplicate of an order from an earlier run gets past intake, which only knows this run. The owner's rule:
// it fails alone, nothing else stops, and the kill switch's cancels go through.
TEST(PgOrderStore, ADuplicateAtTheWriterFailsOnlyItselfAndCancelAllStillGoesThrough) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  {
    PgOrderEventWriter earlier_run(db.url(), nullptr);
    Oms oms(earlier_run, clock, 1);
    const OrderHandle h = oms.create(details(s, 1, 1)).value();
    ASSERT_TRUE(oms.apply(h, E::kRiskReject, {.reason_code = "R-TEST"}));
    wait_written(earlier_run, 2);
  }
  RecordingBroker broker;
  PgOrderEventWriter writer(db.url(), nullptr);
  Oms oms(writer, clock, 8);
  OrderRouter router(oms, writer, broker, Venue::kPaper, {.tag_limit = 20, .capacity = 8},
                     testing::open_limits());

  OrderDetails duplicate = details(s, 2, 5);  // a new order id, client-1 again
  ASSERT_TRUE(duplicate.client_order_id.assign("client-1"));
  const OrderHandle dup = oms.create(duplicate).value();
  ASSERT_TRUE(router.submit(dup, testing::approve(oms.order(dup).details, clock)));

  const OrderHandle other = oms.create(details(s, 3, 5)).value();  // in the same batch or the next
  ASSERT_TRUE(router.submit(other, testing::approve(oms.order(other).details, clock)));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (router.pending() > 0 && std::chrono::steady_clock::now() < deadline) {
    router.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(router.pending(), 0U);
  ASSERT_EQ(broker.sent.size(), 1U);  // the good one went; the duplicate never did
  EXPECT_EQ(broker.sent[0], "client3");
  EXPECT_EQ(oms.order(dup).status, kRiskRejected);
  EXPECT_EQ(oms.order(other).status, kSent);
  EXPECT_EQ(oms.refused_by_storage(), 1U);
  EXPECT_TRUE(writer.healthy());

  ASSERT_TRUE(oms.apply(other, E::kBrokerAck, {.broker_order_id = "B3"}));
  const auto out = router.cancel_all();
  EXPECT_EQ(out.failed, 0U);
  ASSERT_EQ(broker.cancelled.size(), 1U);
  EXPECT_EQ(broker.cancelled[0], "client-3");
  EXPECT_EQ(oms.order(other).status, kCancelPending);
  wait_written(
      writer, 7);  // earlier_run's aren't counted here: 2 refused, then NEW APPROVED SENT OPEN CANCEL_PENDING

  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(
      tx.query_value<std::string>("SELECT status::text FROM \"order\" WHERE client_order_id = 'client-3'"),
      "CANCEL_PENDING");
  EXPECT_EQ(
      tx.query_value<std::string>("SELECT status::text FROM \"order\" WHERE client_order_id = 'client-1'"),
      "RISK_REJECTED");  // the earlier order is untouched
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM \"order\""), 2);
  EXPECT_EQ(tx.query_value<std::string>(
                "SELECT e.payload->'gate'->>'ruleset_version' FROM order_event e JOIN \"order\" o ON o.id = "
                "e.order_id WHERE o.client_order_id = 'client-3' AND e.to_status = 'APPROVED'"),
            "0");  // the gate's stamp, on the APPROVED event
  const auto alert = tx.exec(
                           "SELECT severity, body->>'client_order_id', body->>'sqlstate' FROM alert_event "
                           "WHERE title = 'ORDER_EVENT_REFUSED'")
                         .one_row();
  EXPECT_EQ(alert[0].as<std::string>(), "critical");
  EXPECT_EQ(alert[1].as<std::string>(), "client-1");
  EXPECT_EQ(alert[2].as<std::string>(), "23505");
}

TEST(PgOrderStore, AnOrderWithARowThatCantGoInFailsAlone) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  PgOrderEventWriter writer(db.url(), nullptr);
  Oms oms(writer, clock, 4);
  OrderDetails no_account = details(s, 1, 1);
  no_account.account_id = uuid_of("0190a3b4-7c2d-7e11-8f00-ffffffffffff");  // a foreign key that isn't there
  ASSERT_TRUE(oms.create(no_account));
  ASSERT_TRUE(oms.create(details(s, 2, 1)));
  wait_written(writer, 2);
  EXPECT_TRUE(writer.healthy());
  Uuid refused;
  ASSERT_TRUE(writer.try_pop_refused(refused));
  EXPECT_EQ(refused, no_account.id);
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<std::string>("SELECT client_order_id FROM \"order\""), "client-2");
}

TEST(PgOrderStore, ALiveOrdersEventSurvivesABrokerIdClash) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  const Seeded s = seed(db.url());
  const TickingClock clock;
  PgOrderEventWriter writer(db.url(), nullptr);
  Oms oms(writer, clock, 4);
  const auto open = [&](int n) {
    const OrderHandle h = oms.create(details(s, n, 5)).value();
    EXPECT_TRUE(oms.apply(h, E::kRiskApprove).has_value());
    EXPECT_TRUE(oms.apply(h, E::kSend).has_value());
    EXPECT_TRUE(oms.apply(h, E::kBrokerAck, {.broker_order_id = "SAME"}).has_value());
    return h;
  };
  open(1);
  const OrderHandle second = open(2);
  ASSERT_TRUE(
      oms.apply(second, E::kPartialFill, {.fill_qty = 2, .fill_price = Price::from_units(24'500'000)}));
  wait_written(writer, 9);
  EXPECT_TRUE(writer.healthy());
  pqxx::connection c(db.url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<int>("SELECT filled_qty FROM \"order\" WHERE client_order_id = 'client-2'"), 2);
  EXPECT_EQ(tx.query_value<int>("SELECT count(*) FROM order_event"), 9);  // nothing lost: a fill is a fact
}

TEST(PgOrderStore, ShutdownGivesUpInBoundedTimeAndErrorsDontLeakTheUrl) {
  const std::string dead = "postgres://u:secret-password@127.0.0.1:1/none?connect_timeout=1";
  const auto loaded = load_open_orders(dead);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(std::string(loaded.error().detail).find("secret-password"), std::string::npos);

  const auto start = std::chrono::steady_clock::now();
  {
    PgOrderEventWriter writer(dead, nullptr);
    const OrderEventRecord event;
    ASSERT_TRUE(writer.try_push(event));
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(30));
}

}  // namespace
}  // namespace vyuha::oms

// PgInstrumentDirectory against a fresh, migrated Postgres database (VYUHA_TEST_POSTGRES_URL,
// else skipped); the database is dropped afterwards.

#include "vyuha/session/pg_instrument_directory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "pg_temp_db.h"
#include "require_services.h"

namespace {

using vyuha::testing::admin_url;
using vyuha::testing::TempDb;

TEST(PgInstrumentDirectory, LoadsActiveMappingsAndRefreshes) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const TempDb db(admin);
  {
    pqxx::connection c(db.url());
    pqxx::work tx(c);
    tx.exec(
        R"(INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, lot_size, tick_size, kite_token,
                upstox_key, expiry, valid_from, valid_to) VALUES
      (101, 'NFO', 'FUT', 'NIFTY', 'NIFTY26NOVFUT', 65, 0.1, 15736578, 'NSE_FO|61471', '2026-11-24', '2026-10-28', NULL),
      (102, 'NSE', 'EQ', 'KITEONLY', 'KITEONLY', 1, 0.05, 1990401, NULL, NULL, '2026-10-28', NULL),
      (103, 'NFO', 'FUT', 'NIFTY', 'NIFTY26OCTFUT', 65, 0.1, 999, 'NSE_FO|1', '2026-10-27', '2026-10-01', '2026-10-28'))");
    tx.commit();
  }
  vyuha::session::PgInstrumentDirectory dir(db.url());
  const auto n = dir.refresh();
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 2U);  // the expired contract (valid_to set) is not loaded
  const auto fut = dir.find(101);
  ASSERT_TRUE(fut.has_value());
  EXPECT_EQ(fut.value_or(vyuha::session::BrokerIds{}).kite_token.value_or(0), 15'736'578U);
  EXPECT_EQ(fut.value_or(vyuha::session::BrokerIds{}).upstox_key, "NSE_FO|61471");
  const auto kite_only = dir.find(102);
  ASSERT_TRUE(kite_only.has_value());
  EXPECT_TRUE(
      kite_only.value_or(vyuha::session::BrokerIds{.kite_token = 0, .upstox_key = "x"}).upstox_key.empty());
  EXPECT_FALSE(dir.find(103).has_value());

  vyuha::session::PgInstrumentDirectory bad("postgres://nobody:wrong@127.0.0.1:1/none");
  const auto err = bad.refresh();
  ASSERT_FALSE(err.has_value());
  EXPECT_EQ(err.error().code, vyuha::ErrorCode::kConnectFailed);
  EXPECT_EQ(std::string(err.error().detail).find("wrong"), std::string::npos);  // no credentials echoed
}

}  // namespace

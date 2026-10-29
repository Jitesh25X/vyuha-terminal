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

namespace {

std::string admin_url() {
  const char* u = std::getenv("VYUHA_TEST_POSTGRES_URL");  // NOLINT(concurrency-mt-unsafe): test setup
  return u != nullptr ? u : "";
}

std::string with_db(const std::string& url, const std::string& db) {
  const auto q = url.find('?');
  const auto slash = url.rfind('/', q);
  return url.substr(0, slash + 1) + db + (q == std::string::npos ? "" : url.substr(q));
}

class TempDb {
 public:
  explicit TempDb(std::string admin) : admin_(std::move(admin)) {
    name_ = "vyuha_etest_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100'000'000);
    pqxx::connection c(admin_);
    pqxx::nontransaction(c).exec("CREATE DATABASE " + name_);
    // Same lock as gateway/internal/testdb: 000001 creates a cluster-wide role.
    pqxx::nontransaction(c).exec("SELECT pg_advisory_lock(33347592421209188)");
    pqxx::connection db(url());
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(VYUHA_MIGRATIONS_DIR)) {
      if (e.path().string().ends_with(".up.sql")) {
        files.push_back(e.path());
      }
    }
    std::ranges::sort(files);
    for (const auto& f : files) {
      std::ifstream in(f);
      pqxx::nontransaction(db).exec(std::string(std::istreambuf_iterator<char>(in), {}));
    }
    pqxx::nontransaction(c).exec("SELECT pg_advisory_unlock(33347592421209188)");
  }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  TempDb(TempDb&&) = delete;
  TempDb& operator=(TempDb&&) = delete;
  ~TempDb() {
    try {
      pqxx::connection c(admin_);
      pqxx::nontransaction(c).exec("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE)");
    } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort cleanup in a test fixture
    }
  }
  [[nodiscard]] std::string url() const { return with_db(admin_, name_); }

 private:
  std::string admin_;
  std::string name_;
};

TEST(PgInstrumentDirectory, LoadsActiveMappingsAndRefreshes) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    GTEST_SKIP() << "VYUHA_TEST_POSTGRES_URL not set";
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

// TempDb leaves no database behind: not when a migration fails to apply, not after a normal run.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "pg_temp_db.h"
#include "require_services.h"

namespace vyuha::testing {
namespace {

// How many databases on the server start with `prefix`.
int databases_with(const std::string& admin, const std::string& prefix) {
  pqxx::connection c(admin);
  pqxx::read_transaction tx(c);
  return tx.query_value<int>("SELECT count(*) FROM pg_database WHERE datname LIKE " + tx.quote(prefix + "%"));
}

TEST(TempDb, ADatabaseIsDroppedWhenAMigrationFailsToApply) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const std::string prefix = "vyuha_etest_leakcheck_" + std::to_string(::getpid()) + "_";
  const auto dir = std::filesystem::temp_directory_path() / (prefix + "migrations");
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "000001_broken.up.sql") << "THIS IS NOT SQL;";

  // The constructor throws after CREATE DATABASE succeeded: that is the case the guard exists for.
  EXPECT_THROW(TempDb(admin, dir, prefix), pqxx::sql_error);
  EXPECT_EQ(databases_with(admin, prefix), 0);
  std::filesystem::remove_all(dir);
}

TEST(TempDb, ADatabaseIsDroppedAtTheEndOfANormalRun) {
  const std::string admin = admin_url();
  if (admin.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_POSTGRES_URL not set");
  }
  const std::string prefix = "vyuha_etest_leakcheck_" + std::to_string(::getpid()) + "_ok_";
  {
    const TempDb db(admin, VYUHA_MIGRATIONS_DIR, prefix);
    EXPECT_EQ(databases_with(admin, prefix), 1);
  }
  EXPECT_EQ(databases_with(admin, prefix), 0);
}

}  // namespace
}  // namespace vyuha::testing

#pragma once

// A fresh, migrated Postgres database for one test (VYUHA_TEST_POSTGRES_URL, else the test skips); dropped
// afterwards, and also when its constructor fails after the database exists (a migration that doesn't apply).

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <pqxx/pqxx>

namespace vyuha::testing {

inline std::string admin_url() {
  const char* u = std::getenv("VYUHA_TEST_POSTGRES_URL");  // NOLINT(concurrency-mt-unsafe): test setup
  return u != nullptr ? u : "";
}

inline std::string with_db(const std::string& url, const std::string& db) {
  const auto q = url.find('?');
  const auto slash = url.rfind('/', q);
  return url.substr(0, slash + 1) + db + (q == std::string::npos ? "" : url.substr(q));
}

class TempDb {
 public:
  // `migrations` and `prefix` are seams for pg_temp_db_test.cc only (a broken migration, a name it can look
  // for).
  explicit TempDb(std::string admin, const std::filesystem::path& migrations = VYUHA_MIGRATIONS_DIR,
                  const std::string& prefix = "vyuha_etest_")
      : admin_(std::move(admin)), name_(create(admin_, prefix)), drop_(admin_, name_) {
    // From here on drop_ is a constructed member: if anything below throws, it still drops the database.
    pqxx::connection c(admin_);
    // Same lock as gateway/internal/testdb: 000001 creates a cluster-wide role. It dies with this connection.
    pqxx::nontransaction(c).exec("SELECT pg_advisory_lock(33347592421209188)");
    pqxx::connection db(url());
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(migrations)) {
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
  ~TempDb() = default;  // drop_ drops the database
  [[nodiscard]] std::string url() const { return with_db(admin_, name_); }
  [[nodiscard]] const std::string& name() const { return name_; }

 private:
  // Drops the database it is given when it goes out of scope.
  class Drop {
   public:
    Drop(std::string admin, std::string name) : admin_(std::move(admin)), name_(std::move(name)) {}
    Drop(const Drop&) = delete;
    Drop& operator=(const Drop&) = delete;
    Drop(Drop&&) = delete;
    Drop& operator=(Drop&&) = delete;
    ~Drop() {
      try {
        pqxx::connection c(admin_);
        pqxx::nontransaction(c).exec("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE)");
      } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort cleanup in a test fixture
      }
    }

   private:
    std::string admin_;
    std::string name_;
  };

  static std::string create(const std::string& admin, const std::string& prefix) {
    std::string name =
        prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100'000'000);
    pqxx::connection c(admin);
    pqxx::nontransaction(c).exec("CREATE DATABASE " + name);
    return name;
  }

  std::string admin_;
  std::string name_;
  Drop drop_;
};

}  // namespace vyuha::testing

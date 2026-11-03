// D-18 and the owner's restart rule: kill the engine between persist and send, between send and ack, and
// during recovery. A restart sends nothing, ever: an order the broker has is adopted by its tag, and one it
// lacks expires, with an audit row and an alert. A real SIGKILL of a real process, on a real Postgres.

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "file_broker.h"
#include "pg_temp_db.h"

// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables): POSIX
extern char** environ;

namespace vyuha::oms {
namespace {

using vyuha::testing::admin_url;
using vyuha::testing::CrashPoint;
using vyuha::testing::FileBroker;
using vyuha::testing::TempDb;

struct Seeded {
  std::string account;
  std::string user;
  std::string instrument;
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
  s.instrument = tx.query_value<std::string>(
      "INSERT INTO instrument (exchange, segment, symbol, tradingsymbol, lot_size, tick_size, valid_from) "
      "VALUES ('NSE', 'EQ', 'SYM', 'SYM', 1, 0.05, '2026-01-01') RETURNING id::text");
  tx.commit();
  return s;
}

struct Exit {
  bool killed{false};
  int code{-1};
};

Exit run_child(const std::string& url, const std::string& broker, const std::string& crash,
               const std::string& mode, const Seeded& s) {
  std::vector<std::string> args{VYUHA_OMS_CRASH_CHILD, url, broker, crash, mode, s.account, s.user,
                                s.instrument};
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (::posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
    return {};
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) {
    return {};
  }
  return {.killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          .code = WIFEXITED(status) ? WEXITSTATUS(status) : -1};
}

std::string status_of(const std::string& url) {
  pqxx::connection c(url);
  pqxx::read_transaction tx(c);
  return tx.query_value<std::string>("SELECT status::text FROM \"order\"");
}

int count(const std::string& url, const std::string& sql) {
  pqxx::connection c(url);
  pqxx::read_transaction tx(c);
  return tx.query_value<int>(sql);
}

int with_status(const std::string& url, const std::string& status) {
  return count(url, "SELECT count(*) FROM \"order\" WHERE status = '" + status + "'");
}

int expiry_audits(const std::string& url) {
  return count(url, "SELECT count(*) FROM audit_event WHERE action = 'order.expire'");
}

int expiry_alerts(const std::string& url) {
  return count(url, "SELECT count(*) FROM alert_event WHERE title = 'ORDER_EXPIRED_ON_RESTART'");
}

class OmsCrash : public ::testing::Test {
 protected:
  void SetUp() override {
    if (admin_url().empty()) {
      GTEST_SKIP() << "VYUHA_TEST_POSTGRES_URL not set";
    }
    db_ = std::make_unique<TempDb>(admin_url());
    seeded_ = seed(db_->url());
    broker_path_ = (std::filesystem::temp_directory_path() / (db_->name() + ".broker")).string();
    std::filesystem::remove(broker_path_);
  }
  void TearDown() override {
    if (!broker_path_.empty()) {
      std::filesystem::remove(broker_path_);
    }
  }
  Exit child(const std::string& crash, const std::string& mode) {
    return run_child(db_->url(), broker_path_, crash, mode, seeded_);
  }
  [[nodiscard]] std::vector<std::string> at_broker() const {
    return FileBroker(broker_path_, CrashPoint::kNone).lines();
  }

  [[nodiscard]] std::string url() const { return db_->url(); }

 private:
  std::unique_ptr<TempDb> db_;
  Seeded seeded_;
  std::string broker_path_;
};

TEST_F(OmsCrash, BetweenPersistAndSendRestartExpiresItAndSendsNothing) {
  const Exit crashed = child("before_send", "place");
  ASSERT_TRUE(crashed.killed) << "exit " << crashed.code;
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(status_of(url()), "APPROVED");  // durable before the adapter was called

  const Exit restarted = child("none", "recover");
  EXPECT_EQ(restarted.code, 0);
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(status_of(url()), "EXPIRED");
  EXPECT_EQ(expiry_audits(url()), 1);
  EXPECT_EQ(expiry_alerts(url()), 1);

  EXPECT_EQ(child("none", "recover").code, 0);  // a second restart changes nothing
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(status_of(url()), "EXPIRED");
  EXPECT_EQ(expiry_audits(url()), 1);
  EXPECT_EQ(expiry_alerts(url()), 1);
}

// The one way an expired order could fire: a caller replaying the same order id after a restart, which intake
// can't catch, since it only knows this run. It's refused at the writer, and the next restart still works.
TEST_F(OmsCrash, ReplayingAnExpiredOrderSendsNothingAndTheNextRestartWorks) {
  ASSERT_TRUE(child("before_send", "place").killed);
  ASSERT_EQ(child("none", "recover").code, 0);
  ASSERT_EQ(status_of(url()), "EXPIRED");

  EXPECT_EQ(child("none", "place").code, 0);
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(status_of(url()), "EXPIRED");
  EXPECT_EQ(count(url(), "SELECT count(*) FROM order_event"), 3);
  EXPECT_EQ(count(url(), "SELECT count(*) FROM alert_event WHERE title = 'ORDER_EVENT_REFUSED'"), 1);

  EXPECT_EQ(child("none", "recover").code, 0);
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(status_of(url()), "EXPIRED");
}

// This child dies the moment anything calls send, so a clean exit proves recovery never tried.
TEST_F(OmsCrash, RecoveryNeverCallsSend) {
  ASSERT_TRUE(child("before_send", "place").killed);
  const Exit restarted = child("before_send", "recover");
  EXPECT_FALSE(restarted.killed);
  EXPECT_EQ(restarted.code, 0);
  EXPECT_EQ(status_of(url()), "EXPIRED");
}

TEST_F(OmsCrash, WithOrdersQueuedRestartSendsNoneOfThem) {
  ASSERT_TRUE(child("before_send", "place_three").killed);
  EXPECT_EQ(with_status(url(), "APPROVED"), 3);
  EXPECT_EQ(child("before_send", "recover").code, 0);
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(with_status(url(), "EXPIRED"), 3);
  EXPECT_EQ(expiry_audits(url()), 3);
  EXPECT_EQ(expiry_alerts(url()), 3);
}

// The first of three reached the broker when the engine died: that one is adopted, and the others expire.
TEST_F(OmsCrash, AfterOneOfThreeWentOutRestartAdoptsItAndExpiresTheRest) {
  ASSERT_TRUE(child("after_send", "place_three").killed);
  ASSERT_EQ(at_broker().size(), 1U);
  EXPECT_EQ(with_status(url(), "APPROVED"), 3);
  EXPECT_EQ(child("before_send", "recover").code, 0);
  EXPECT_EQ(at_broker().size(), 1U);
  EXPECT_EQ(with_status(url(), "OPEN"), 1);
  EXPECT_EQ(with_status(url(), "EXPIRED"), 2);
  EXPECT_EQ(expiry_audits(url()), 2);
  EXPECT_EQ(expiry_alerts(url()), 2);
  EXPECT_EQ(count(url(), "SELECT count(*) FROM \"order\" WHERE broker_order_id = 'B-" + at_broker().front() +
                             "' AND status = 'OPEN'"),
            1);
}

TEST_F(OmsCrash, BetweenSendAndAckRestartFindsItByTagAndDoesntSendAgain) {
  const Exit crashed = child("after_send", "place");
  ASSERT_TRUE(crashed.killed) << "exit " << crashed.code;
  ASSERT_EQ(at_broker().size(), 1U);
  EXPECT_EQ(status_of(url()),
            "APPROVED");  // SENT is recorded after the adapter has it: this is the worst case

  const Exit restarted = child("none", "recover");
  EXPECT_EQ(restarted.code, 0);
  EXPECT_EQ(at_broker().size(), 1U);
  EXPECT_EQ(status_of(url()), "OPEN");  // adopted, with the broker's id
  EXPECT_EQ(expiry_audits(url()), 0);
  EXPECT_EQ(expiry_alerts(url()), 0);
  pqxx::connection c(url());
  pqxx::read_transaction tx(c);
  EXPECT_EQ(tx.query_value<std::string>("SELECT broker_order_id FROM \"order\""), "B-" + at_broker().front());
}

// The expiry may or may not have reached Postgres when the second crash hit. Either way it ends the same,
// once.
TEST_F(OmsCrash, CrashingAgainDuringRecoveryStillExpiresItOnce) {
  ASSERT_TRUE(child("before_send", "place").killed);
  ASSERT_TRUE(child("after_recover", "recover").killed);
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(child("before_send", "recover").code, 0);
  EXPECT_TRUE(at_broker().empty());
  EXPECT_EQ(status_of(url()), "EXPIRED");
  EXPECT_EQ(expiry_audits(url()), 1);
  EXPECT_EQ(expiry_alerts(url()), 1);
}

TEST_F(OmsCrash, CrashingAgainDuringAdoptionStillLeavesOne) {
  ASSERT_TRUE(child("after_send", "place").killed);
  ASSERT_TRUE(child("after_recover", "recover").killed);
  EXPECT_EQ(at_broker().size(), 1U);
  EXPECT_EQ(child("before_send", "recover").code, 0);
  EXPECT_EQ(at_broker().size(), 1U);
  EXPECT_EQ(status_of(url()), "OPEN");
  EXPECT_EQ(expiry_audits(url()), 0);
}

TEST_F(OmsCrash, WithoutACrashItGoesOutOnce) {
  EXPECT_EQ(child("none", "place").code, 0);
  EXPECT_EQ(at_broker().size(), 1U);
  EXPECT_EQ(status_of(url()), "SENT");
}

}  // namespace
}  // namespace vyuha::oms

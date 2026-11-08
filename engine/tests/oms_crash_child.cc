// The engine side of the crash test: places one or three orders (or, with `recover`, recovers), and dies by
// SIGKILL at the chosen point. Usage: oms_crash_child URL BROKER_FILE
// before_send|after_send|after_recover|none place|place_three|recover ACCOUNT USER INSTRUMENT

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "file_broker.h"
#include "test_gate.h"
#include "vyuha/core/clock.h"
#include "vyuha/oms/pg_order_store.h"

namespace {

using vyuha::oms::OrderDetails;
using vyuha::oms::Uuid;

constexpr std::array<std::string_view, 3> kClientOrderIds{"0190a3b4-7c2d-7e11-8f00-aabbccddeeff",
                                                          "0190a3b4-7c2d-7e11-8f00-aabbccddee00",
                                                          "0190a3b4-7c2d-7e11-8f00-aabbccddee01"};
constexpr std::array<std::string_view, 3> kOrderIds{"0190a3b4-7c2d-7e11-8f00-000000000001",
                                                    "0190a3b4-7c2d-7e11-8f00-000000000002",
                                                    "0190a3b4-7c2d-7e11-8f00-000000000003"};
constexpr std::size_t kTagLimit = 20;

int fail(std::string_view why) {
  std::cerr << why << "\n";
  return 2;
}

bool place(std::span<char*> args, std::size_t orders, const vyuha::WallClock& clock, vyuha::oms::Oms& oms,
           vyuha::oms::OrderRouter& router) {
  for (std::size_t i = 0; i < orders; ++i) {
    OrderDetails d;
    d.id = Uuid::parse(kOrderIds.at(i)).value_or(Uuid{});
    if (!d.client_order_id.assign(kClientOrderIds.at(i)) || !d.tag.assign("manual")) {
      return false;
    }
    d.account_id = Uuid::parse(args[5]).value_or(Uuid{});
    d.user_id = Uuid::parse(args[6]).value_or(Uuid{});
    d.instrument_id = std::strtoll(args[7], nullptr, 10);
    d.qty = 1;
    d.price = vyuha::Price::from_units(10'000);
    d.created_at = clock.now();
    const auto handle = oms.create(d);
    if (!handle.has_value() ||
        !router.submit(*handle, vyuha::testing::approve(oms.order(*handle).details, clock)).has_value()) {
      return false;
    }
  }
  return true;
}

bool recover_all(const std::string& url, vyuha::oms::Oms& oms, vyuha::oms::OrderRouter& router,
                 vyuha::oms::IBrokerOrderBook& book) {
  const auto open = vyuha::oms::load_open_orders(url);
  if (!open.has_value()) {
    return false;
  }
  for (const auto& order : *open) {
    const auto handle = oms.restore(order);
    if (!handle.has_value()) {
      return false;
    }
    static_cast<void>(router.recover(*handle, book));
  }
  return true;
}

int run(std::span<char*> args) {
  const std::string url = args[1];
  const std::string_view crash = args[3];
  const bool recover = std::string_view(args[4]) == "recover";
  auto point = vyuha::testing::CrashPoint::kNone;
  if (crash == "before_send") {
    point = vyuha::testing::CrashPoint::kBeforeSend;
  } else if (crash == "after_send") {
    point = vyuha::testing::CrashPoint::kAfterSend;
  }
  vyuha::testing::FileBroker broker(args[2], point);
  const vyuha::WallClock clock;
  vyuha::oms::PgOrderEventWriter writer(url);
  vyuha::oms::Oms oms(writer, clock, 8);
  vyuha::oms::OrderRouter router(oms, writer, broker, vyuha::oms::Venue::kPaper,
                                 {.tag_limit = kTagLimit, .capacity = 8});

  if (recover) {
    if (!recover_all(url, oms, router, broker)) {
      return fail("couldn't recover");
    }
    if (crash == "after_recover") {
      std::raise(SIGKILL);  // NOLINT(cert-err33-c): it doesn't return
    }
  } else if (!place(args, std::string_view(args[4]) == "place_three" ? kOrderIds.size() : 1, clock, oms,
                    router)) {
    return fail("couldn't submit");
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  // All of them durable before the first send, so a crash leaves every queued order APPROVED in Postgres.
  while (writer.written() < writer.pushed() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (router.pending() > 0 && std::chrono::steady_clock::now() < deadline) {
    router.poll();  // the point of a crash is reached in here
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (writer.written() < writer.pushed() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return router.pending() == 0 ? 0 : fail("still pending");
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): a test helper: an escaped exception fails the test
int main(int argc, char** argv) {
  constexpr int kArgs = 8;
  if (argc != kArgs) {
    return fail("usage");
  }
  return run(std::span<char*>(argv, static_cast<std::size_t>(argc)));
}

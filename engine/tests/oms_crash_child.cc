// The engine side of the crash test: places (or, with `recover`, recovers) one order, and dies by SIGKILL at
// the chosen point. Usage: oms_crash_child URL BROKER_FILE before_send|after_send|none place|recover ACCOUNT
// USER INSTRUMENT

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "file_broker.h"
#include "vyuha/core/clock.h"
#include "vyuha/oms/pg_order_store.h"

namespace {

using vyuha::oms::OrderDetails;
using vyuha::oms::OrderEvent;
using vyuha::oms::Uuid;

constexpr std::string_view kClientOrderId = "0190a3b4-7c2d-7e11-8f00-aabbccddeeff";
constexpr std::size_t kTagLimit = 20;

int fail(std::string_view why) {
  std::cerr << why << "\n";
  return 2;
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
  vyuha::oms::OrderRouter router(oms, writer, broker, kTagLimit, 8);

  if (recover) {
    const auto open = vyuha::oms::load_open_orders(url);
    if (!open.has_value()) {
      return fail("load_open_orders failed");
    }
    for (const auto& order : *open) {
      const auto handle = oms.restore(order);
      if (!handle.has_value()) {
        return fail("restore failed");
      }
      static_cast<void>(router.recover(*handle, broker));
    }
  } else {
    OrderDetails d;
    d.id = Uuid::parse("0190a3b4-7c2d-7e11-8f00-000000000001").value_or(Uuid{});
    if (!d.client_order_id.assign(kClientOrderId) || !d.tag.assign("manual")) {
      return fail("bad order");
    }
    d.account_id = Uuid::parse(args[5]).value_or(Uuid{});
    d.user_id = Uuid::parse(args[6]).value_or(Uuid{});
    d.instrument_id = std::strtoll(args[7], nullptr, 10);
    d.qty = 1;
    d.price = vyuha::Price::from_units(10'000);
    d.created_at = clock.now();
    const auto handle = oms.create(d);
    if (!handle.has_value() || !oms.apply(*handle, OrderEvent::kRiskApprove).has_value() ||
        !router.submit(*handle).has_value()) {
      return fail("couldn't submit");
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
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

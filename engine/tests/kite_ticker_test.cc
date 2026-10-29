// F-MD-2 AC2 (fixture-based): a local WebSocket server replays recorded-format frames, drops the
// first connection, and the ticker reconnects from the bound source IP, resubscribes everything,
// keeps decoding into the core's ring and logs the gap. No live network (invariant 3).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "vyuha/broker/kite/ticker.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace kite = vyuha::broker::kite;
using tcp = asio::ip::tcp;

std::vector<std::uint8_t> fixture(const char* name) {
  std::ifstream f(std::filesystem::path(VYUHA_FIXTURES_DIR) / "vcr" / "kite" / name, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

// Wakes a server thread blocked in accept() so it can see its stop flag (Asio objects must not be
// closed from another thread while in use).
void wake(std::uint16_t port) {
  asio::io_context io;
  tcp::socket s(io);
  boost::system::error_code ec;
  s.connect({asio::ip::make_address("127.0.0.1"), port}, ec);
}

// Accepts connections one at a time on 127.0.0.1. Each connection: read the expected number of
// subscription text frames, record them and the client's source address, send the fixture frames;
// the first connection is then dropped, later ones stay open until the test ends.
class FixtureServer {
 public:
  explicit FixtureServer(std::size_t expected_msgs) : expected_(expected_msgs) {
    acceptor_.open(tcp::v4());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind({asio::ip::make_address("127.0.0.1"), 0});
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] { serve(); });
  }
  FixtureServer(const FixtureServer&) = delete;
  FixtureServer& operator=(const FixtureServer&) = delete;
  FixtureServer(FixtureServer&&) = delete;
  FixtureServer& operator=(FixtureServer&&) = delete;
  // NOLINTNEXTLINE(bugprone-exception-escape): test fixture; a throwing join should abort the test
  ~FixtureServer() {
    stop_ = true;
    wake(port_);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::vector<std::vector<std::string>> subscriptions() {
    const std::scoped_lock lock(mu_);
    return subs_;
  }
  [[nodiscard]] std::vector<std::string> targets() {
    const std::scoped_lock lock(mu_);
    return targets_;
  }

 private:
  void serve() {
    const auto full = fixture("full_mixed.bin");
    const auto ltp = fixture("ltp_only.bin");
    const auto heartbeat = fixture("heartbeat.bin");
    for (int conn = 0; !stop_; ++conn) {
      boost::system::error_code ec;
      tcp::socket sock(io_);
      acceptor_.accept(sock, ec);
      if (ec || stop_) {
        return;
      }
      beast::websocket::stream<tcp::socket> ws(std::move(sock));
      beast::http::request<beast::http::empty_body> req;
      beast::flat_buffer hbuf;
      beast::http::read(ws.next_layer(), hbuf, req, ec);
      if (ec) {
        continue;
      }
      ws.accept(req, ec);
      if (ec) {
        continue;
      }
      std::vector<std::string> got;
      for (std::size_t i = 0; i < expected_; ++i) {
        beast::flat_buffer buf;
        ws.read(buf, ec);
        if (ec) {
          break;
        }
        got.push_back(beast::buffers_to_string(buf.data()));
      }
      {
        const std::scoped_lock lock(mu_);
        subs_.push_back(got);
        targets_.emplace_back(req.target());
      }
      ws.binary(true);
      ws.write(asio::buffer(full), ec);
      ws.write(asio::buffer(heartbeat), ec);
      ws.write(asio::buffer(ltp), ec);
      if (conn == 0) {
        ws.next_layer().close(ec);  // abrupt drop, no close frame
        continue;
      }
      while (!stop_ && !ec) {  // keep the second connection alive with heartbeats
        ws.write(asio::buffer(heartbeat), ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }

  asio::io_context io_;
  tcp::acceptor acceptor_{io_};
  std::size_t expected_;
  std::uint16_t port_{0};
  std::atomic<bool> stop_{false};
  std::mutex mu_;
  std::vector<std::vector<std::string>> subs_;
  std::vector<std::string> targets_;
  std::thread thread_;
};

constexpr std::uint32_t kNfoFut = (61471U << 8U) | 2U;
constexpr std::uint32_t kNseEq = (2885U << 8U) | 1U;
constexpr std::uint32_t kNifty = 256265;

kite::TickerConfig config(std::uint16_t port) {
  kite::TickerConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = std::to_string(port);
  cfg.tls = false;
  cfg.api_key = "test_key";
  cfg.access_token = vyuha::SecretRef("test_token");
  cfg.source_ip = "127.0.0.1";
  cfg.backoff = {.initial = 20'000'000, .cap = 200'000'000};  // 20 ms → 200 ms for the test
  cfg.jitter_seed = 1;
  cfg.feed_id = 7;
  return cfg;
}

TEST(KiteTicker, ReconnectsResubscribesAndLogsTheGap) {
  const std::vector<kite::Subscription> subs = {
      {kNfoFut, kite::Mode::kFull}, {kNseEq, kite::Mode::kQuote}, {kNifty, kite::Mode::kFull}};
  FixtureServer server(3);  // subscribe + quote mode + full mode
  const vyuha::WallClock clock;
  auto ring = std::make_unique<vyuha::TickRing>();
  kite::KiteTicker ticker(config(server.port()), kite::TokenMap({{kNfoFut, 1}, {kNseEq, 2}, {kNifty, 3}}),
                          *ring, clock);
  ASSERT_TRUE(ticker.start(subs).has_value());
  EXPECT_EQ(ticker.shard_count(), 1U);

  // Two connections × (3 ticks from full_mixed + 2 from ltp_only).
  std::vector<vyuha::md::Tick> got;
  vyuha::md::Tick t;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (got.size() < 10 && std::chrono::steady_clock::now() < deadline) {
    while (ring->try_pop(t)) {
      got.push_back(t);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ticker.stop();
  ASSERT_EQ(got.size(), 10U);
  EXPECT_EQ(got[0].instrument_id, 1U);
  EXPECT_EQ(got[5].instrument_id, 1U);  // same stream again after the reconnect
  EXPECT_TRUE(std::ranges::all_of(got, [](const auto& t) { return t.feed_id == 7U; }));  // ADR-009 routing

  const auto stats = ticker.stats();
  EXPECT_EQ(stats.connects, 2U);
  EXPECT_GE(stats.disconnects, 1U);
  EXPECT_EQ(stats.ticks, 10U);
  EXPECT_EQ(stats.ring_full, 0U);

  const auto subs_seen = server.subscriptions();
  ASSERT_EQ(subs_seen.size(), 2U);
  EXPECT_EQ(subs_seen[0], subs_seen[1]);  // full resubscription
  EXPECT_EQ(subs_seen[0].front(), R"({"a":"subscribe","v":[)" + std::to_string(kNfoFut) + "," +
                                      std::to_string(kNseEq) + "," + std::to_string(kNifty) + "]}");
  EXPECT_EQ(server.targets()[0], "/?api_key=test_key&access_token=test_token");

  const auto gaps = ticker.gaps();
  ASSERT_EQ(gaps.size(), 1U);
  EXPECT_EQ(gaps[0].shard, 0U);
  EXPECT_GT(gaps[0].end, gaps[0].start);
  EXPECT_GE(gaps[0].end - gaps[0].start, 10'000'000);  // at least the 20 ms ±jitter backoff
}

TEST(KiteTicker, RefusesToStartWithoutTheStaticIpOrOverLimits) {
  const vyuha::WallClock clock;
  auto ring = std::make_unique<vyuha::TickRing>();
  auto cfg = config(1);
  cfg.source_ip = "203.0.113.7";
  kite::KiteTicker not_local(std::move(cfg), kite::TokenMap{}, *ring, clock);
  const auto r = not_local.start(std::vector<kite::Subscription>{{kNseEq, kite::Mode::kLtp}});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, vyuha::ErrorCode::kStaticIpNotOnHost);

  auto small = config(1);
  small.limits = {.max_per_connection = 1, .max_connections = 1};
  kite::KiteTicker over(std::move(small), kite::TokenMap{}, *ring, clock);
  const auto r2 =
      over.start(std::vector<kite::Subscription>{{kNseEq, kite::Mode::kLtp}, {kNifty, kite::Mode::kLtp}});
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error().code, vyuha::ErrorCode::kCapacityExceeded);
}

}  // namespace

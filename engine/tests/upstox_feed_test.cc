// F-MD-2 AC2 for Upstox (fixture-based, no live network): a local "authorize" HTTP server hands out
// single-use ws:// URLs; a local WebSocket server rejects reused codes, records the binary
// subscription requests, sends market_info + live_feed frames and drops the first connection.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include "vyuha/broker/upstox/feed.h"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace up = vyuha::broker::upstox;
using tcp = asio::ip::tcp;

std::string fixture(const char* name) {
  std::ifstream f(std::filesystem::path(VYUHA_FIXTURES_DIR) / "vcr" / "upstox" / name, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

tcp::acceptor listen_local(asio::io_context& io) {
  tcp::acceptor a(io);
  a.open(tcp::v4());
  a.set_option(asio::socket_base::reuse_address(true));
  a.bind({asio::ip::make_address("127.0.0.1"), 0});
  a.listen();
  return a;
}

// Wakes a server thread blocked in accept() so it can see its stop flag (Asio objects must not be
// closed from another thread while in use).
void wake(std::uint16_t port) {
  asio::io_context io;
  tcp::socket s(io);
  boost::system::error_code ec;
  s.connect({asio::ip::make_address("127.0.0.1"), port}, ec);
}

class FixtureServers {
 public:
  FixtureServers()
      : http_port_(http_acceptor_.local_endpoint().port()), ws_port_(ws_acceptor_.local_endpoint().port()) {
    http_thread_ = std::thread([this] { serve_authorize(); });
    ws_thread_ = std::thread([this] { serve_feed(); });
  }
  FixtureServers(const FixtureServers&) = delete;
  FixtureServers& operator=(const FixtureServers&) = delete;
  FixtureServers(FixtureServers&&) = delete;
  FixtureServers& operator=(FixtureServers&&) = delete;
  // NOLINTNEXTLINE(bugprone-exception-escape): test fixture; a throwing join should abort the test
  ~FixtureServers() {
    stop_ = true;
    wake(http_port_);
    wake(ws_port_);
    http_thread_.join();
    ws_thread_.join();
  }

  [[nodiscard]] std::uint16_t http_port() const { return http_port_; }
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes):
  // test observations
  std::atomic<int> authorizations{0};
  std::atomic<int> bad_auth_headers{0};
  std::atomic<int> reused_codes{0};
  std::atomic<int> text_requests{0};
  std::mutex mu;
  std::vector<std::vector<std::string>> requests;  // per accepted connection
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes)

 private:
  void serve_authorize() {
    while (!stop_) {
      tcp::socket sock(http_io_);
      boost::system::error_code ec;
      http_acceptor_.accept(sock, ec);
      if (ec || stop_) {
        return;
      }
      beast::flat_buffer buf;
      http::request<http::empty_body> req;
      http::read(sock, buf, req, ec);
      if (ec) {
        continue;
      }
      if (req[http::field::authorization] != "Bearer test_token") {
        ++bad_auth_headers;
      }
      const int n = ++authorizations;
      http::response<http::string_body> res{http::status::ok, 11};
      res.set(http::field::content_type, "application/json");
      res.body() = R"({"status":"success","data":{"authorized_redirect_uri":"ws://127.0.0.1:)" +
                   std::to_string(ws_port_) + "/feed?requestId=r&code=c" + std::to_string(n) + "\"}}";
      res.prepare_payload();
      http::write(sock, res, ec);
    }
  }

  void serve_feed() {
    const auto info = fixture("market_info.bin");
    const auto live = fixture("live_feed.bin");
    for (int conn = 0; !stop_; ++conn) {
      tcp::socket sock(ws_io_);
      boost::system::error_code ec;
      ws_acceptor_.accept(sock, ec);
      if (ec || stop_) {
        return;
      }
      beast::websocket::stream<tcp::socket> ws(std::move(sock));
      http::request<http::empty_body> req;
      beast::flat_buffer hbuf;
      http::read(ws.next_layer(), hbuf, req, ec);
      const std::string target(req.target());
      if (ec || !used_codes_.insert(target).second) {
        ++reused_codes;  // the code in the URL is single-use
        continue;
      }
      ws.accept(req, ec);
      if (ec) {
        continue;
      }
      std::vector<std::string> got;
      beast::flat_buffer buf;
      ws.read(buf, ec);  // one sub request (full mode) for this test's subscription set
      if (!ws.got_binary()) {
        ++text_requests;
      }
      got.push_back(beast::buffers_to_string(buf.data()));
      {
        const std::scoped_lock lock(mu);
        requests.push_back(got);
      }
      ws.binary(true);
      ws.write(asio::buffer(info), ec);
      ws.write(asio::buffer(live), ec);
      if (conn == 0) {
        ws.next_layer().close(ec);  // abrupt drop
        continue;
      }
      while (!stop_ && !ec) {
        ws.ping({}, ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }

  asio::io_context http_io_;
  asio::io_context ws_io_;
  tcp::acceptor http_acceptor_{listen_local(http_io_)};
  tcp::acceptor ws_acceptor_{listen_local(ws_io_)};
  std::uint16_t http_port_;
  std::uint16_t ws_port_;
  std::atomic<bool> stop_{false};
  std::set<std::string> used_codes_;
  std::thread http_thread_;
  std::thread ws_thread_;
};

up::FeedConfig config(std::uint16_t http_port) {
  up::FeedConfig cfg;
  cfg.authorize_host = "127.0.0.1";
  cfg.authorize_port = std::to_string(http_port);
  cfg.tls = false;
  cfg.access_token = vyuha::SecretRef("test_token");
  cfg.source_ip = "127.0.0.1";
  cfg.backoff = {.initial = 20'000'000, .cap = 200'000'000};
  cfg.jitter_seed = 3;
  cfg.feed_id = 9;
  return cfg;
}

TEST(UpstoxFeed, AuthorizesEachConnectionResubscribesAndLogsTheGap) {
  FixtureServers servers;
  const vyuha::WallClock clock;
  auto ring = std::make_unique<vyuha::TickRing>();
  up::UpstoxFeed feed(
      config(servers.http_port()),
      up::KeyMap({{"NSE_FO|61471", 1}, {"NSE_INDEX|Nifty 50", 2}, {"NSE_EQ|INE002A01018", 3}}), *ring, clock);
  const std::vector<up::Subscription> subs = {{"NSE_FO|61471", up::Mode::kFull},
                                              {"NSE_INDEX|Nifty 50", up::Mode::kFull},
                                              {"NSE_EQ|INE002A01018", up::Mode::kFull}};
  ASSERT_TRUE(feed.start(subs).has_value());

  std::vector<vyuha::md::Tick> got;
  vyuha::md::Tick t;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (got.size() < 6 && std::chrono::steady_clock::now() < deadline) {
    while (ring->try_pop(t)) {
      got.push_back(t);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  feed.stop();
  ASSERT_EQ(got.size(), 6U);  // 3 instruments × 2 connections
  // D-26: stamped from the monotonic clock on arrival, which a wall-clock reading could not satisfy.
  const vyuha::Nanos after = clock.monotonic();
  EXPECT_TRUE(std::ranges::all_of(got, [after](const auto& t) {
    return t.recv_mono > 0 && t.recv_mono <= after && after - t.recv_mono < 60 * vyuha::kNanosPerSecond;
  }));
  EXPECT_TRUE(std::ranges::all_of(got, [](const auto& t) { return t.feed_id == 9U; }));  // ADR-009 routing

  const auto stats = feed.stats();
  EXPECT_EQ(stats.connects, 2U);
  EXPECT_EQ(stats.authorizations, 2U);  // a fresh single-use URL per connection
  EXPECT_EQ(stats.market_info, 2U);
  EXPECT_EQ(stats.ticks, 6U);
  EXPECT_EQ(servers.reused_codes.load(), 0);
  EXPECT_EQ(servers.bad_auth_headers.load(), 0);
  EXPECT_EQ(servers.text_requests.load(), 0);  // requests go as binary frames

  const std::scoped_lock lock(servers.mu);
  ASSERT_EQ(servers.requests.size(), 2U);
  EXPECT_EQ(servers.requests[0], servers.requests[1]);
  EXPECT_NE(servers.requests[0].at(0).find(R"("mode":"full")"), std::string::npos);

  const auto gaps = feed.gaps();
  ASSERT_EQ(gaps.size(), 1U);
  EXPECT_GT(gaps[0].end, gaps[0].start);
}

TEST(UpstoxFeed, RefusesStaticIpNotOnHostAndOverLimits) {
  const vyuha::WallClock clock;
  auto ring = std::make_unique<vyuha::TickRing>();
  auto cfg = config(1);
  cfg.source_ip = "203.0.113.7";
  up::UpstoxFeed feed(std::move(cfg), up::KeyMap{}, *ring, clock);
  const std::vector<up::Subscription> one = {{"NSE_EQ|X", up::Mode::kLtpc}};
  EXPECT_EQ(feed.start(one).error().code, vyuha::ErrorCode::kStaticIpNotOnHost);

  up::UpstoxFeed d30(config(1), up::KeyMap{}, *ring, clock);
  const std::vector<up::Subscription> deep = {{"NSE_EQ|X", up::Mode::kFullD30}};  // Standard plan
  EXPECT_EQ(d30.start(deep).error().code, vyuha::ErrorCode::kInvalidArgument);
}

}  // namespace

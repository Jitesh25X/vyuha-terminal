#include "feed_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "quill/LogMacros.h"
#include "vyuha/util/log.h"
#include "vyuha/util/net.h"

namespace vyuha::broker {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr auto kNoThrow = asio::as_tuple(asio::use_awaitable);
constexpr auto kHandshakeTimeout = std::chrono::seconds(10);
constexpr std::size_t kMaxMessage = std::size_t{4} << 20U;  // 4 MiB

std::chrono::nanoseconds to_chrono(Nanos n) { return std::chrono::nanoseconds(n); }

void load_ca(asio::ssl::context& ctx, const std::string& ca_file) {
  if (!ca_file.empty()) {
    ctx.load_verify_file(ca_file);
    return;
  }
  // vcpkg's OpenSSL has no system trust store configured: use the OS bundle.
  constexpr std::array<const char*, 3> kBundles{"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem",
                                                "/etc/pki/tls/certs/ca-bundle.crt"};
  for (const char* path : kBundles) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      ctx.load_verify_file(path);
      return;
    }
  }
  ctx.set_default_verify_paths();
}

struct ShardState {
  Backoff backoff;
  std::optional<Nanos> down_since;  // set while the shard is disconnected
  Nanos last_frame{0};
};

}  // namespace

struct FeedClient::Impl final : HttpGetter {
  FeedClientConfig cfg;
  FeedProtocol& protocol;
  const IClock& clock;
  std::vector<ShardState> state;

  asio::io_context io;
  std::unique_ptr<asio::ssl::context> tls;
  std::thread thread;
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> connects{0}, disconnects{0}, frames{0};
  mutable std::mutex gaps_mu;
  std::vector<Gap> gap_log;

  Impl(FeedClientConfig c, FeedProtocol& p, const IClock& clk) : cfg(std::move(c)), protocol(p), clock(clk) {}

  void record_gap(std::size_t shard, Nanos start, Nanos end) {
    {
      const std::scoped_lock lock(gaps_mu);
      gap_log.push_back(Gap{shard, start, end});
    }
    if (quill::Logger* log = log::engine()) {
      LOG_WARNING(log, "{} feed gap: shard={} start_ns={} end_ns={} duration_ms={}", cfg.name, shard, start,
                  end, (end - start) / 1'000'000);
    }
  }

  // TCP connection to host:port from the configured source IP (bind before connect).
  asio::awaitable<std::optional<beast::tcp_stream>> connect_bound(std::string host, std::string port) const {
    auto ex = co_await asio::this_coro::executor;
    tcp::resolver resolver(ex);
    auto [rec, endpoints] = co_await resolver.async_resolve(host, port, kNoThrow);
    boost::system::error_code aec;
    const auto source = asio::ip::make_address(cfg.source_ip, aec);
    if (rec || aec) {
      co_return std::nullopt;
    }
    beast::tcp_stream stream(ex);
    for (const auto& entry : endpoints) {
      const auto ep = entry.endpoint();
      if (ep.address().is_v4() != source.is_v4()) {
        continue;
      }
      boost::system::error_code ec;
      auto& sock = stream.socket();
      if (sock.is_open()) {
        sock.close(ec);
      }
      sock.open(ep.protocol(), ec);
      if (!ec) {
        sock.bind(tcp::endpoint(source, 0), ec);  // invariant 4
      }
      if (ec) {
        continue;
      }
      stream.expires_after(kHandshakeTimeout);
      auto [cec] = co_await stream.async_connect(ep, kNoThrow);
      if (!cec) {
        stream.expires_never();
        co_return std::optional<beast::tcp_stream>(std::move(stream));
      }
    }
    co_return std::nullopt;
  }

  // SNI + hostname verification before the TLS handshake.
  static bool prepare_tls(beast::ssl_stream<beast::tcp_stream>& s, const std::string& host) {
    if (SSL_set_tlsext_host_name(s.native_handle(), host.c_str()) != 1) {
      return false;
    }
    s.set_verify_callback(asio::ssl::host_name_verification(host));
    beast::get_lowest_layer(s).expires_after(kHandshakeTimeout);
    return true;
  }

  template <class Stream>
  asio::awaitable<std::expected<std::string, Error>> http_exchange(Stream s, FeedEndpoint ep) {
    http::request<http::empty_body> req{http::verb::get, ep.target, 11};
    req.set(http::field::host, ep.host);
    req.set(http::field::user_agent, "vyuha-engine");
    for (const auto& [k, v] : ep.headers) {
      req.set(k, v);
    }
    beast::get_lowest_layer(s).expires_after(kHandshakeTimeout);
    auto [wec, wn] = co_await http::async_write(s, req, kNoThrow);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    if (!wec) {
      auto [rec, rn] = co_await http::async_read(s, buf, res, kNoThrow);
      wec = rec;
    }
    if (wec) {
      co_return std::unexpected(Error{ErrorCode::kConnectFailed, "http: request failed"});
    }
    if (res.result_int() < 200 || res.result_int() >= 300) {
      co_return std::unexpected(Error{ErrorCode::kConnectFailed, "http: non-2xx response"});
    }
    co_return std::move(res.body());
  }

  asio::awaitable<std::expected<std::string, Error>> get(FeedEndpoint ep) override {
    auto tcp_stream = co_await connect_bound(ep.host, ep.port);
    if (!tcp_stream.has_value()) {
      co_return std::unexpected(Error{ErrorCode::kConnectFailed, "http: connect failed"});
    }
    if (!ep.tls) {
      co_return co_await http_exchange(std::move(*tcp_stream), std::move(ep));
    }
    if (tls == nullptr) {
      co_return std::unexpected(Error{ErrorCode::kConnectFailed, "http: TLS not configured"});
    }
    beast::ssl_stream<beast::tcp_stream> s(std::move(*tcp_stream), *tls);
    if (!prepare_tls(s, ep.host)) {
      co_return std::unexpected(Error{ErrorCode::kConnectFailed, "http: TLS setup failed"});
    }
    auto [hec] = co_await s.async_handshake(asio::ssl::stream_base::client, kNoThrow);
    beast::get_lowest_layer(s).expires_never();
    if (hec) {
      co_return std::unexpected(Error{ErrorCode::kConnectFailed, "http: TLS handshake failed"});
    }
    co_return co_await http_exchange(std::move(s), std::move(ep));
  }

  // Owns an open WebSocket transport: handshake, subscribe, stream frames until it drops.
  template <class Stream>
  asio::awaitable<void> run_websocket(Stream wsock, std::size_t shard, FeedEndpoint ep) {
    ShardState& st = state[shard];
    ws::stream_base::timeout to{};
    to.handshake_timeout = kHandshakeTimeout;
    to.idle_timeout = to_chrono(cfg.idle_timeout);
    to.keep_alive_pings = true;
    wsock.set_option(to);
    wsock.read_message_max(kMaxMessage);
    wsock.set_option(ws::stream_base::decorator([headers = ep.headers](ws::request_type& req) {
      for (const auto& [k, v] : headers) {
        req.set(k, v);
      }
    }));
    auto [hec] = co_await wsock.async_handshake(ep.host, ep.target, kNoThrow);
    std::ranges::fill(ep.target, '\0');  // targets can carry tokens: don't leave them in freed memory
    if (hec) {
      co_return;
    }
    connects.fetch_add(1, std::memory_order_relaxed);
    wsock.binary(protocol.binary_requests());
    for (const std::string& msg : protocol.subscribe_messages(shard)) {
      auto [wec, n] = co_await wsock.async_write(asio::buffer(msg), kNoThrow);
      if (wec) {
        co_return;
      }
    }
    beast::flat_buffer buf;
    bool first = true;
    for (;;) {
      auto [ec, n] = co_await wsock.async_read(buf, kNoThrow);
      if (ec) {
        co_return;
      }
      const Nanos now = clock.now();
      st.last_frame = now;
      if (first) {
        // Healthy again: close the gap and let the next outage start from the short delay.
        if (st.down_since.has_value()) {
          record_gap(shard, *st.down_since, now);
          st.down_since.reset();
        }
        st.backoff.reset();
        first = false;
      }
      frames.fetch_add(1, std::memory_order_relaxed);
      const auto data = buf.cdata();
      protocol.on_frame(
          shard, wsock.got_binary(),
          std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.data()), data.size()), now);
      buf.consume(buf.size());
    }
  }

  asio::awaitable<void> connect_once(std::size_t shard) {
    auto ep = co_await protocol.endpoint(shard, *this);
    if (!ep.has_value()) {
      if (quill::Logger* log = log::engine()) {
        LOG_WARNING(log, "{} feed shard {}: endpoint unavailable: {}", cfg.name, shard, ep.error().detail);
      }
      co_return;
    }
    auto tcp_stream = co_await connect_bound(ep->host, ep->port);
    if (!tcp_stream.has_value()) {
      co_return;
    }
    if (!ep->tls) {
      co_await run_websocket(ws::stream<beast::tcp_stream>(std::move(*tcp_stream)), shard, std::move(*ep));
      co_return;
    }
    if (tls == nullptr) {
      co_return;
    }
    ws::stream<beast::ssl_stream<beast::tcp_stream>> wsock(std::move(*tcp_stream), *tls);
    if (!prepare_tls(wsock.next_layer(), ep->host)) {
      co_return;
    }
    auto [hec] = co_await wsock.next_layer().async_handshake(asio::ssl::stream_base::client, kNoThrow);
    beast::get_lowest_layer(wsock).expires_never();
    if (hec) {
      co_return;
    }
    co_await run_websocket(std::move(wsock), shard, std::move(*ep));
  }

  asio::awaitable<void> shard_loop(std::size_t shard) {
    auto ex = co_await asio::this_coro::executor;
    ShardState& st = state[shard];
    asio::steady_timer timer(ex);
    while (!stopping.load(std::memory_order_acquire)) {
      co_await connect_once(shard);
      if (stopping.load(std::memory_order_acquire)) {
        break;
      }
      disconnects.fetch_add(1, std::memory_order_relaxed);
      if (!st.down_since.has_value()) {
        st.down_since = st.last_frame != 0 ? st.last_frame : clock.now();
      }
      const Nanos delay = st.backoff.next();
      if (quill::Logger* log = log::engine()) {
        LOG_WARNING(log, "{} feed shard {} disconnected; reconnect attempt {} in {} ms", cfg.name, shard,
                    st.backoff.attempts(), delay / 1'000'000);
      }
      timer.expires_after(to_chrono(delay));
      co_await timer.async_wait(kNoThrow);
    }
  }
};

FeedClient::FeedClient(FeedClientConfig cfg, FeedProtocol& protocol, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), protocol, clock)) {}

FeedClient::~FeedClient() { stop(); }

std::expected<void, Error> FeedClient::start(std::size_t shards) {
  if (impl_->thread.joinable()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "feed: already started"});
  }
  if (auto ip = net::check_ip_on_host(impl_->cfg.source_ip); !ip) {
    return std::unexpected(ip.error());
  }
  impl_->state.clear();
  for (std::size_t i = 0; i < shards; ++i) {
    const std::uint64_t seed =
        impl_->cfg.jitter_seed != 0 ? impl_->cfg.jitter_seed + i : std::random_device{}();
    impl_->state.push_back(ShardState{Backoff(impl_->cfg.backoff, seed), std::nullopt, 0});
  }
  if (impl_->cfg.tls) {
    impl_->tls = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_client);
    impl_->tls->set_verify_mode(asio::ssl::verify_peer);
    try {
      load_ca(*impl_->tls, impl_->cfg.ca_file);
    } catch (const boost::system::system_error&) {
      return std::unexpected(Error{ErrorCode::kConnectFailed, "feed: cannot load CA certificates"});
    }
  }
  for (std::size_t i = 0; i < shards; ++i) {
    asio::co_spawn(impl_->io, impl_->shard_loop(i), asio::detached);
  }
  impl_->thread = std::thread([impl = impl_.get()] { impl->io.run(); });
  return {};
}

void FeedClient::stop() {
  if (!impl_ || !impl_->thread.joinable()) {
    return;
  }
  impl_->stopping.store(true, std::memory_order_release);
  impl_->io.stop();
  impl_->thread.join();
}

FeedConnStats FeedClient::stats() const noexcept {
  return FeedConnStats{impl_->connects.load(std::memory_order_relaxed),
                       impl_->disconnects.load(std::memory_order_relaxed),
                       impl_->frames.load(std::memory_order_relaxed)};
}

std::vector<Gap> FeedClient::gaps() const {
  const std::scoped_lock lock(impl_->gaps_mu);
  return impl_->gap_log;
}

}  // namespace vyuha::broker

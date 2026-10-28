#include "vyuha/broker/kite/ticker.h"

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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <simdjson.h>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "quill/LogMacros.h"
#include "vyuha/util/log.h"
#include "vyuha/util/net.h"

namespace vyuha::broker::kite {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr auto kNoThrow = asio::as_tuple(asio::use_awaitable);
constexpr auto kHandshakeTimeout = std::chrono::seconds(10);

std::chrono::nanoseconds to_chrono(Nanos n) { return std::chrono::nanoseconds(n); }

// Text frames are JSON {"type": "order"|"error"|"message", "data": ...}. Rare (postbacks), so
// the copy into a padded buffer is acceptable here; ticks never take this path.
std::string_view text_kind(std::string_view text, simdjson::ondemand::parser& parser) {
  const simdjson::padded_string padded(text);
  auto doc = parser.iterate(padded);
  std::string_view type;
  if (doc.error() != simdjson::SUCCESS || doc["type"].get_string().get(type) != simdjson::SUCCESS) {
    return "unparsable";
  }
  if (type == "order" || type == "error" || type == "message") {
    return type;
  }
  return "unknown";
}

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

}  // namespace

// Per-shard reconnect state, owned by Impl so coroutines take only the shard index.
struct ShardState {
  Backoff backoff;
  std::optional<Nanos> down_since;  // set while the shard is disconnected
  Nanos last_frame{0};
};

struct KiteTicker::Impl {
  TickerConfig cfg;
  TokenMap tokens;
  TickRing& out;
  const IClock& clock;
  std::vector<Shard> shards;
  std::vector<ShardState> state;

  asio::io_context io;
  std::unique_ptr<asio::ssl::context> tls;
  std::thread thread;
  std::atomic<bool> stopping{false};

  // Counters: written on the IO thread, read anywhere.
  std::atomic<std::uint64_t> connects{0}, disconnects{0}, frames{0}, ticks{0}, ring_full{0}, text_frames{0},
      unknown_token{0}, malformed{0};
  mutable std::mutex gaps_mu;
  std::vector<Gap> gap_log;

  Impl(TickerConfig c, TokenMap t, TickRing& r, const IClock& clk)
      : cfg(std::move(c)), tokens(std::move(t)), out(r), clock(clk) {}

  void record_gap(std::size_t shard, Nanos start, Nanos end) {
    {
      const std::scoped_lock lock(gaps_mu);
      gap_log.push_back(Gap{shard, start, end});
    }
    if (quill::Logger* log = log::engine()) {
      LOG_WARNING(log, "kite ticker gap: shard={} start_ns={} end_ns={} duration_ms={}", shard, start, end,
                  (end - start) / 1'000'000);
    }
  }

  void on_frame(std::size_t shard, bool binary, std::span<const std::uint8_t> bytes, Nanos now,
                ParseStats& ps, simdjson::ondemand::parser& json) {
    frames.fetch_add(1, std::memory_order_relaxed);
    if (binary) {
      const ParseStats before = ps;
      parse_frame(bytes, tokens, now, ps, [this](const md::Tick& t) noexcept {
        if (!out.try_push(t)) {
          ring_full.fetch_add(1, std::memory_order_relaxed);
        }
      });
      ticks.fetch_add(ps.ticks - before.ticks, std::memory_order_relaxed);
      unknown_token.fetch_add(ps.unknown_token - before.unknown_token, std::memory_order_relaxed);
      malformed.fetch_add(ps.malformed - before.malformed, std::memory_order_relaxed);
      return;
    }
    text_frames.fetch_add(1, std::memory_order_relaxed);
    const std::string_view kind = text_kind(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), json);  // NOLINT
    // Order postbacks reach the OMS in M3; errors/messages are surfaced in the log now (kind only).
    if (kind != "order") {
      if (quill::Logger* log = log::engine()) {
        LOG_WARNING(log, "kite ticker text frame: shard={} kind={}", shard, kind);
      }
    }
  }

  // Owns an open (TCP or TLS) WebSocket: handshake, subscribe, stream frames until it drops.
  template <class Stream>
  asio::awaitable<void> run_websocket(Stream wsock, std::size_t shard) {
    ShardState& st = state[shard];
    ws::stream_base::timeout to{};
    to.handshake_timeout = kHandshakeTimeout;
    to.idle_timeout = to_chrono(cfg.idle_timeout);
    to.keep_alive_pings = true;
    wsock.set_option(to);
    wsock.read_message_max(4U * 1024U * 1024U);
    std::string target = "/?api_key=" + cfg.api_key + "&access_token=";
    target += cfg.access_token.reveal();
    auto [hec] = co_await wsock.async_handshake(cfg.host, target, kNoThrow);
    std::ranges::fill(target, '\0');  // don't leave the token in a freed heap block
    if (hec) {
      co_return;
    }
    connects.fetch_add(1, std::memory_order_relaxed);
    wsock.text(true);
    for (const std::string& msg : subscribe_messages(shards[shard])) {
      auto [wec, n] = co_await wsock.async_write(asio::buffer(msg), kNoThrow);
      if (wec) {
        co_return;
      }
    }
    beast::flat_buffer buf;
    simdjson::ondemand::parser json;
    ParseStats ps;
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
      const auto data = buf.cdata();
      on_frame(shard, wsock.got_binary(),
               std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.data()), data.size()), now,
               ps, json);
      buf.consume(buf.size());
    }
  }

  // One connection attempt: TCP from source_ip, optional TLS, then the WebSocket session.
  asio::awaitable<void> connect_once(std::size_t shard) {
    auto ex = co_await asio::this_coro::executor;
    tcp::resolver resolver(ex);
    auto [rec, endpoints] = co_await resolver.async_resolve(cfg.host, cfg.port, kNoThrow);
    if (rec) {
      co_return;
    }
    boost::system::error_code aec;
    const auto source = asio::ip::make_address(cfg.source_ip, aec);
    if (aec) {
      co_return;
    }
    beast::tcp_stream tcp_stream(ex);
    bool connected = false;
    for (const auto& entry : endpoints) {
      const auto ep = entry.endpoint();
      if (ep.address().is_v4() != source.is_v4()) {
        continue;
      }
      boost::system::error_code ec;
      auto& sock = tcp_stream.socket();
      if (sock.is_open()) {
        sock.close(ec);
      }
      sock.open(ep.protocol(), ec);
      if (!ec) {
        sock.bind(tcp::endpoint(source, 0), ec);  // invariant 4: bind before connect
      }
      if (ec) {
        continue;
      }
      tcp_stream.expires_after(kHandshakeTimeout);
      auto [cec] = co_await tcp_stream.async_connect(ep, kNoThrow);
      if (!cec) {
        connected = true;
        break;
      }
    }
    if (!connected) {
      co_return;
    }
    tcp_stream.expires_never();  // websocket timeouts take over
    if (!cfg.tls) {
      co_await run_websocket(ws::stream<beast::tcp_stream>(std::move(tcp_stream)), shard);
      co_return;
    }
    if (tls == nullptr) {
      co_return;
    }
    ws::stream<beast::ssl_stream<beast::tcp_stream>> wsock(std::move(tcp_stream), *tls);
    if (SSL_set_tlsext_host_name(wsock.next_layer().native_handle(), cfg.host.c_str()) != 1) {
      co_return;
    }
    wsock.next_layer().set_verify_callback(asio::ssl::host_name_verification(cfg.host));
    beast::get_lowest_layer(wsock).expires_after(kHandshakeTimeout);
    auto [sec] = co_await wsock.next_layer().async_handshake(asio::ssl::stream_base::client, kNoThrow);
    if (sec) {
      co_return;
    }
    beast::get_lowest_layer(wsock).expires_never();
    co_await run_websocket(std::move(wsock), shard);
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
        LOG_WARNING(log, "kite ticker shard {} disconnected; reconnect attempt {} in {} ms", shard,
                    st.backoff.attempts(), delay / 1'000'000);
      }
      timer.expires_after(to_chrono(delay));
      co_await timer.async_wait(kNoThrow);
    }
  }
};

KiteTicker::KiteTicker(TickerConfig cfg, TokenMap tokens, TickRing& out, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), std::move(tokens), out, clock)) {}

KiteTicker::~KiteTicker() { stop(); }

std::expected<void, Error> KiteTicker::start(std::span<const Subscription> subs) {
  if (impl_->thread.joinable()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "kite ticker: already started"});
  }
  if (auto ip = net::check_ip_on_host(impl_->cfg.source_ip); !ip) {
    return std::unexpected(ip.error());
  }
  auto plan = plan_shards(subs, impl_->cfg.limits);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  impl_->shards = std::move(*plan);
  impl_->state.clear();
  for (std::size_t i = 0; i < impl_->shards.size(); ++i) {
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
      return std::unexpected(Error{ErrorCode::kConnectFailed, "kite ticker: cannot load CA certificates"});
    }
  }
  for (std::size_t i = 0; i < impl_->shards.size(); ++i) {
    asio::co_spawn(impl_->io, impl_->shard_loop(i), asio::detached);
  }
  impl_->thread = std::thread([impl = impl_.get()] { impl->io.run(); });
  return {};
}

void KiteTicker::stop() {
  if (!impl_ || !impl_->thread.joinable()) {
    return;
  }
  impl_->stopping.store(true, std::memory_order_release);
  impl_->io.stop();
  impl_->thread.join();
}

TickerStats KiteTicker::stats() const noexcept {
  const auto r = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  return TickerStats{r(impl_->connects),      r(impl_->disconnects), r(impl_->frames),
                     r(impl_->ticks),         r(impl_->ring_full),   r(impl_->text_frames),
                     r(impl_->unknown_token), r(impl_->malformed)};
}

std::vector<Gap> KiteTicker::gaps() const {
  const std::scoped_lock lock(impl_->gaps_mu);
  return impl_->gap_log;
}

std::size_t KiteTicker::shard_count() const noexcept { return impl_->shards.size(); }

}  // namespace vyuha::broker::kite

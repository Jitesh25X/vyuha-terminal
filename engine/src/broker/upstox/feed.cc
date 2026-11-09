#include "vyuha/broker/upstox/feed.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <simdjson.h>
#include <boost/asio/awaitable.hpp>

#include "../common/feed_client.h"
#include "quill/LogMacros.h"
#include "vyuha/util/log.h"

namespace vyuha::broker::upstox {

std::expected<WsUrl, Error> parse_ws_url(std::string_view url) {
  WsUrl out;
  if (url.starts_with("wss://")) {
    url.remove_prefix(6);
    out.tls = true;
  } else if (url.starts_with("ws://")) {
    url.remove_prefix(5);
    out.tls = false;
  } else {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "upstox: feed URL is not ws:// or wss://"});
  }
  const std::size_t slash = url.find('/');
  const std::string_view authority = url.substr(0, slash);
  out.target = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    out.host = std::string(authority);
    out.port = out.tls ? "443" : "80";
  } else {
    out.host = std::string(authority.substr(0, colon));
    out.port = std::string(authority.substr(colon + 1));
  }
  if (out.host.empty() || out.port.empty()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "upstox: feed URL has no host"});
  }
  return out;
}

struct UpstoxFeed::Impl final : FeedProtocol {
  FeedConfig cfg;
  KeyMap keys;
  TickRing& out;
  std::vector<Subscription> subs;
  FeedClient client;
  DecodeStats ds;  // IO thread only
  std::atomic<std::uint64_t> authorizations{0}, ticks{0}, ring_full{0}, market_info{0}, unknown_key{0},
      bad_price{0}, malformed{0};

  Impl(FeedConfig c, KeyMap k, TickRing& r, const IClock& clock)
      : cfg(std::move(c)),
        keys(std::move(k)),
        out(r),
        client(FeedClientConfig{.name = "upstox",
                                .source_ip = cfg.source_ip,
                                .backoff = cfg.backoff,
                                .idle_timeout = cfg.idle_timeout,
                                .ca_file = cfg.ca_file,
                                .jitter_seed = cfg.jitter_seed,
                                .tls = cfg.tls},
               *this, clock) {}

  // Every connection needs a fresh URL: the `code` in it is single-use.
  // FeedClient (the HttpGetter) outlives this coroutine.
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  boost::asio::awaitable<std::expected<FeedEndpoint, Error>> endpoint(std::size_t /*shard*/,
                                                                      HttpGetter& http) override {
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
    std::string bearer = "Bearer ";
    bearer += cfg.access_token.reveal();
    auto body = co_await http.get(
        FeedEndpoint{.host = cfg.authorize_host,
                     .port = cfg.authorize_port,
                     .target = cfg.authorize_path,
                     .tls = cfg.tls,
                     .headers = {{"Authorization", std::move(bearer)}, {"Accept", "application/json"}}});
    if (!body.has_value()) {
      co_return std::unexpected(body.error());
    }
    authorizations.fetch_add(1, std::memory_order_relaxed);
    simdjson::ondemand::parser parser;
    const simdjson::padded_string padded(*body);
    auto doc = parser.iterate(padded);
    std::string_view uri;
    if (doc.error() != simdjson::SUCCESS ||
        doc["data"]["authorized_redirect_uri"].get_string().get(uri) != simdjson::SUCCESS) {
      co_return std::unexpected(
          Error{ErrorCode::kConnectFailed, "upstox: authorize response has no redirect URI"});
    }
    auto url = parse_ws_url(uri);
    if (!url.has_value()) {
      co_return std::unexpected(url.error());
    }
    co_return FeedEndpoint{
        .host = url->host, .port = url->port, .target = url->target, .tls = url->tls, .headers = {}};
  }

  [[nodiscard]] std::vector<std::string> subscribe_messages(std::size_t /*shard*/) const override {
    return upstox::subscribe_messages(subs);
  }

  [[nodiscard]] bool binary_requests() const noexcept override { return true; }  // Upstox requires binary

  void on_frame(std::size_t /*shard*/, bool binary, std::span<const std::uint8_t> bytes, Nanos now,
                Nanos recv_mono) override {
    if (!binary) {
      return;  // the feed is protobuf only
    }
    const DecodeStats before = ds;
    decode_frame(bytes, keys, now, ds, [this, recv_mono](const md::Tick& t) noexcept {
      md::Tick stamped = t;
      stamped.feed_id = cfg.feed_id;
      stamped.recv_mono = recv_mono;
      if (!out.try_push(stamped)) {
        ring_full.fetch_add(1, std::memory_order_relaxed);
      }
    });
    ticks.fetch_add(ds.ticks - before.ticks, std::memory_order_relaxed);
    market_info.fetch_add(ds.market_info - before.market_info, std::memory_order_relaxed);
    unknown_key.fetch_add(ds.unknown_key - before.unknown_key, std::memory_order_relaxed);
    bad_price.fetch_add(ds.bad_price - before.bad_price, std::memory_order_relaxed);
    malformed.fetch_add(ds.malformed - before.malformed, std::memory_order_relaxed);
  }
};

UpstoxFeed::UpstoxFeed(FeedConfig cfg, KeyMap keys, TickRing& out, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), std::move(keys), out, clock)) {}

UpstoxFeed::~UpstoxFeed() { stop(); }

std::expected<void, Error> UpstoxFeed::start(std::span<const Subscription> subs) {
  auto planned = plan_subscriptions(subs, impl_->cfg.plan);
  if (!planned) {
    return std::unexpected(planned.error());
  }
  impl_->subs = std::move(*planned);
  return impl_->client.start(1);  // limits are per user: one connection carries the whole set
}

void UpstoxFeed::stop() {
  if (impl_) {
    impl_->client.stop();
  }
}

FeedStats UpstoxFeed::stats() const noexcept {
  const auto r = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  const FeedConnStats c = impl_->client.stats();
  return FeedStats{c.connects,          c.disconnects,       r(impl_->authorizations), c.frames,
                   r(impl_->ticks),     r(impl_->ring_full), r(impl_->market_info),    r(impl_->unknown_key),
                   r(impl_->bad_price), r(impl_->malformed)};
}

std::vector<Gap> UpstoxFeed::gaps() const { return impl_->client.gaps(); }

}  // namespace vyuha::broker::upstox

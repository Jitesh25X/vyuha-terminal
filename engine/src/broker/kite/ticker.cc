#include "vyuha/broker/kite/ticker.h"

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

namespace vyuha::broker::kite {

namespace {

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

}  // namespace

// Kite's half of the feed: endpoint with credentials in the query, JSON text requests, binary
// tick frames decoded straight into the core's ring.
struct KiteTicker::Impl final : FeedProtocol {
  TickerConfig cfg;
  TokenMap tokens;
  TickRing& out;
  std::vector<Shard> shards;
  FeedClient client;
  simdjson::ondemand::parser json;
  ParseStats ps;  // IO thread only
  std::atomic<std::uint64_t> ticks{0}, ring_full{0}, text_frames{0}, unknown_token{0}, malformed{0};

  Impl(TickerConfig c, TokenMap t, TickRing& r, const IClock& clock)
      : cfg(std::move(c)),
        tokens(std::move(t)),
        out(r),
        client(FeedClientConfig{.name = "kite",
                                .source_ip = cfg.source_ip,
                                .backoff = cfg.backoff,
                                .idle_timeout = cfg.idle_timeout,
                                .ca_file = cfg.ca_file,
                                .jitter_seed = cfg.jitter_seed,
                                .tls = cfg.tls},
               *this, clock) {}

  // FeedClient (the HttpGetter) outlives this coroutine.
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  boost::asio::awaitable<std::expected<FeedEndpoint, Error>> endpoint(std::size_t /*shard*/,
                                                                      HttpGetter& /*http*/) override {
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
    std::string target = "/?api_key=" + cfg.api_key + "&access_token=";
    target += cfg.access_token.reveal();
    co_return FeedEndpoint{
        .host = cfg.host, .port = cfg.port, .target = std::move(target), .tls = cfg.tls, .headers = {}};
  }

  [[nodiscard]] std::vector<std::string> subscribe_messages(std::size_t shard) const override {
    return kite::subscribe_messages(shards.at(shard));
  }

  [[nodiscard]] bool binary_requests() const noexcept override { return false; }

  void on_frame(std::size_t shard, bool binary, std::span<const std::uint8_t> bytes, Nanos now) override {
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
};

KiteTicker::KiteTicker(TickerConfig cfg, TokenMap tokens, TickRing& out, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), std::move(tokens), out, clock)) {}

KiteTicker::~KiteTicker() { stop(); }

std::expected<void, Error> KiteTicker::start(std::span<const Subscription> subs) {
  auto plan = plan_shards(subs, impl_->cfg.limits);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  impl_->shards = std::move(*plan);
  return impl_->client.start(impl_->shards.size());
}

void KiteTicker::stop() {
  if (impl_) {
    impl_->client.stop();
  }
}

TickerStats KiteTicker::stats() const noexcept {
  const auto r = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  const FeedConnStats c = impl_->client.stats();
  return TickerStats{c.connects,
                     c.disconnects,
                     c.frames,
                     r(impl_->ticks),
                     r(impl_->ring_full),
                     r(impl_->text_frames),
                     r(impl_->unknown_token),
                     r(impl_->malformed)};
}

std::vector<Gap> KiteTicker::gaps() const { return impl_->client.gaps(); }

std::size_t KiteTicker::shard_count() const noexcept { return impl_->shards.size(); }

}  // namespace vyuha::broker::kite

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "vyuha/core/clock.h"
#include "vyuha/core/error.h"
#include "vyuha/md/tick.h"

namespace vyuha::bus {

struct PublisherConfig {
  std::string url{"nats://127.0.0.1:4222"};
  std::string kv_bucket{"md_last"};
  Nanos kv_min_interval{250'000'000};  // latest snapshot per instrument, at most this often
  std::size_t max_instruments{50'000};
};

struct PublisherStats {
  std::uint64_t enqueued_ticks{0};
  std::uint64_t enqueued_bars{0};
  std::uint64_t dropped{0};  // ring full: publisher behind; the core never waits
  std::uint64_t published_ticks{0};
  std::uint64_t published_bars{0};
  std::uint64_t kv_puts{0};
  std::uint64_t publish_errors{0};
};

// Market-data sink for the core loop: on_tick/on_bar only copy into a bounded SPSC ring (core
// thread = sole producer). A publisher thread encodes protobuf and publishes to NATS:
//   md.tick.<instrument_id>          vyuha.marketdata.v1.Tick
//   md.bar.<1s|1m>.<instrument_id>   vyuha.marketdata.v1.Bar (final bars)
//   $KV.<bucket>.<instrument_id>     vyuha.marketdata.v1.Snapshot (JetStream KV, history 1)
// published_ticks is the "engine-published count" the recorder's row count is checked against.
class MdPublisher final : public md::IMdSink {
 public:
  MdPublisher(PublisherConfig cfg, const IClock& clock);
  MdPublisher(const MdPublisher&) = delete;
  MdPublisher& operator=(const MdPublisher&) = delete;
  MdPublisher(MdPublisher&&) = delete;
  MdPublisher& operator=(MdPublisher&&) = delete;
  ~MdPublisher() override;

  // Connects, ensures the KV bucket exists, starts the publisher thread.
  [[nodiscard]] std::expected<void, Error> start();
  // Drains what's queued (bounded wait), then disconnects.
  void stop();

  void on_tick(const md::Tick& tick) noexcept override;
  void on_bar(const md::Bar& bar) noexcept override;

  [[nodiscard]] PublisherStats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vyuha::bus

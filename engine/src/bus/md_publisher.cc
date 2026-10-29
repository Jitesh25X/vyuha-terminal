#include "vyuha/bus/md_publisher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include <nats/nats.h>

#include "quill/LogMacros.h"
#include "vyuha/bus/md_encode.h"
#include "vyuha/core/spsc_ring.h"
#include "vyuha/md/id_map.h"
#include "vyuha/util/log.h"

namespace vyuha::bus {

namespace {

struct Msg {
  bool is_bar{false};
  md::Tick tick;
  md::Bar bar;
};
using MsgRing = SpscRing<Msg, 1U << 15U>;

// "<prefix><id>" into a fixed buffer, NUL-terminated (cnats wants C strings). No allocation.
template <std::size_t N>
const char* subject(std::array<char, N>& buf, std::string_view prefix, std::uint64_t id) noexcept {
  const std::size_t n = std::min(prefix.size(), N - 22);  // room for 20 digits + NUL
  std::copy_n(prefix.data(), n, buf.data());
  const auto r = std::to_chars(buf.data() + n, buf.data() + N - 1, id);
  *r.ptr = '\0';
  return buf.data();
}

}  // namespace

struct MdPublisher::Impl {
  PublisherConfig cfg;
  const IClock& clock;
  std::unique_ptr<MsgRing> ring = std::make_unique<MsgRing>();
  md::IdMap<Nanos> last_kv;  // publisher thread only
  natsConnection* nc{nullptr};
  jsCtx* js{nullptr};
  kvStore* kv{nullptr};
  std::jthread thread;
  std::atomic<std::uint64_t> enqueued_ticks{0}, enqueued_bars{0}, dropped{0}, published_ticks{0},
      published_bars{0}, kv_puts{0}, publish_errors{0};

  Impl(PublisherConfig c, const IClock& clk) : cfg(std::move(c)), clock(clk), last_kv(cfg.max_instruments) {}

  ~Impl() { close(); }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  void close() noexcept {
    if (kv != nullptr) {
      kvStore_Destroy(kv);
      kv = nullptr;
    }
    if (js != nullptr) {
      jsCtx_Destroy(js);
      js = nullptr;
    }
    if (nc != nullptr) {
      natsConnection_FlushTimeout(nc, 2000);
      natsConnection_Destroy(nc);
      nc = nullptr;
    }
  }

  void publish(const char* subj, std::span<const std::uint8_t> data,
               std::atomic<std::uint64_t>& ok) noexcept {
    if (data.empty() ||
        natsConnection_Publish(nc, subj, data.data(), static_cast<int>(data.size())) != NATS_OK) {
      publish_errors.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ok.fetch_add(1, std::memory_order_relaxed);
  }

  void handle(const Msg& m, std::array<std::uint8_t, kMaxSnapshotBytes>& buf,
              std::array<char, 96>& subj) noexcept {
    if (m.is_bar) {
      const std::size_t n = encode_bar(m.bar, buf);
      publish(subject(subj, m.bar.interval == md::BarInterval::k1s ? "md.bar.1s." : "md.bar.1m.",
                      m.bar.instrument_id),
              std::span<const std::uint8_t>(buf.data(), n), published_bars);
      return;
    }
    const std::size_t n = encode_tick(m.tick, buf);
    publish(subject(subj, "md.tick.", m.tick.instrument_id), std::span<const std::uint8_t>(buf.data(), n),
            published_ticks);
    // Latest snapshot to the KV bucket, throttled per instrument. A plain publish to the KV
    // stream's subject: no round trip on this thread (KV readers see the newest revision).
    Nanos* last = last_kv.find_or_insert(m.tick.instrument_id);
    const Nanos now = clock.now();
    if (last != nullptr && now - *last >= cfg.kv_min_interval) {
      *last = now;
      const std::size_t sn = encode_snapshot(m.tick, false, buf);
      const std::string_view prefix = kv_prefix;
      publish(subject(subj, prefix, m.tick.instrument_id), std::span<const std::uint8_t>(buf.data(), sn),
              kv_puts);
    }
  }

  void run(const std::stop_token& st) noexcept {
    std::array<std::uint8_t, kMaxSnapshotBytes> buf{};
    std::array<char, 96> subj{};
    Msg m;
    for (;;) {
      if (ring->try_pop(m)) {
        handle(m, buf, subj);
        continue;
      }
      if (st.stop_requested()) {
        return;  // ring drained
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

  std::string kv_prefix;
};

MdPublisher::MdPublisher(PublisherConfig cfg, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), clock)) {}

MdPublisher::~MdPublisher() { stop(); }

std::expected<void, Error> MdPublisher::start() {
  if (impl_->thread.joinable()) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "publisher: already started"});
  }
  if (natsConnection_ConnectTo(&impl_->nc, impl_->cfg.url.c_str()) != NATS_OK) {
    impl_->close();
    return std::unexpected(Error{ErrorCode::kConnectFailed, "publisher: cannot connect to NATS"});
  }
  if (natsConnection_JetStream(&impl_->js, impl_->nc, nullptr) != NATS_OK) {
    impl_->close();
    return std::unexpected(Error{ErrorCode::kConnectFailed, "publisher: JetStream unavailable"});
  }
  kvConfig kc;
  kvConfig_Init(&kc);
  kc.Bucket = impl_->cfg.kv_bucket.c_str();
  kc.History = 1;
  if (js_KeyValue(&impl_->kv, impl_->js, kc.Bucket) != NATS_OK &&
      js_CreateKeyValue(&impl_->kv, impl_->js, &kc) != NATS_OK) {
    impl_->close();
    return std::unexpected(Error{ErrorCode::kConnectFailed, "publisher: cannot open KV bucket"});
  }
  impl_->kv_prefix = "$KV." + impl_->cfg.kv_bucket + ".";
  impl_->thread = std::jthread([impl = impl_.get()](const std::stop_token& st) { impl->run(st); });
  if (quill::Logger* log = log::engine()) {
    LOG_INFO(log, "md publisher connected; kv bucket {}", impl_->cfg.kv_bucket);
  }
  return {};
}

void MdPublisher::stop() {
  if (!impl_) {
    return;
  }
  if (impl_->thread.joinable()) {
    impl_->thread.request_stop();
    impl_->thread.join();
  }
  impl_->close();
}

void MdPublisher::on_tick(const md::Tick& tick) noexcept {
  Msg m;
  m.tick = tick;
  if (impl_->ring->try_push(m)) {
    impl_->enqueued_ticks.fetch_add(1, std::memory_order_relaxed);
  } else {
    impl_->dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void MdPublisher::on_bar(const md::Bar& bar) noexcept {
  Msg m;
  m.is_bar = true;
  m.bar = bar;
  if (impl_->ring->try_push(m)) {
    impl_->enqueued_bars.fetch_add(1, std::memory_order_relaxed);
  } else {
    impl_->dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

PublisherStats MdPublisher::stats() const noexcept {
  const auto r = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  return PublisherStats{r(impl_->enqueued_ticks),  r(impl_->enqueued_bars),  r(impl_->dropped),
                        r(impl_->published_ticks), r(impl_->published_bars), r(impl_->kv_puts),
                        r(impl_->publish_errors)};
}

}  // namespace vyuha::bus

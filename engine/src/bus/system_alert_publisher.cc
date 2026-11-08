#include "vyuha/bus/system_alert_publisher.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <nats/nats.h>
#include <quill/LogMacros.h>

#include "vyuha/bus/md_encode.h"
#include "vyuha/util/log.h"

namespace vyuha::bus {

namespace {

constexpr std::string_view kCode = "ORDER_LOG_OUTAGE";
constexpr std::string_view kSeverity = "critical";
constexpr std::string_view kBanner = "ORDER LOG DOWN: new orders blocked";
constexpr Nanos kNanosPerMilli = 1'000'000;
constexpr int kReconnectWaitMs = 500;
constexpr int kStopFlushMs = 500;
constexpr std::size_t kMaxChanges = 32;

struct Change {
  bool active{false};
  std::int64_t since_ms{0};
};

// With a callback, a failed first connect returns at once and keeps retrying in the background.
void on_connected(natsConnection* /*nc*/, void* /*closure*/) {}

}  // namespace

struct SystemAlertPublisher::Impl {
  Impl(SystemAlertConfig c, const IClock& clk) : cfg(std::move(c)), clock(&clk) {}

  SystemAlertConfig cfg;
  const IClock* clock;
  natsOptions* opts{nullptr};
  natsConnection* nc{nullptr};
  std::thread thread;

  std::mutex mu;
  std::condition_variable cv;
  std::array<Change, kMaxChanges> changes{};
  std::size_t change_count{0};
  std::optional<std::int64_t> active_since;
  bool stopping{false};

  std::atomic<std::uint64_t> published{0};
  std::atomic<std::uint64_t> errors{0};
  bool failing{false};  // publisher thread only: one log line per run of failures

  void add_change(Change c) noexcept {  // caller holds mu
    // Full only if the thread is stuck. The latest state is the one that matters.
    change_count = change_count < changes.size() ? change_count + 1 : changes.size();
    changes.at(change_count - 1) = c;
  }

  // NOLINTNEXTLINE(bugprone-exception-escape): Quill logging
  void publish(Change c) noexcept {
    std::array<std::uint8_t, kMaxSystemAlertBytes> buf{};
    const std::size_t n = encode_system_alert(
        {.code = kCode, .severity = kSeverity, .banner = kBanner, .active = c.active, .since_ms = c.since_ms},
        buf);
    // Not while disconnected: the reconnect buffer would replay an outage that's over. The repeat covers a
    // skipped start, and the gateway's banner TTL a skipped end.
    if (n != 0 && natsConnection_Status(nc) == NATS_CONN_STATUS_CONNECTED &&
        natsConnection_Publish(nc, cfg.subject.c_str(), buf.data(), static_cast<int>(n)) == NATS_OK) {
      published.fetch_add(1, std::memory_order_relaxed);
      failing = false;
      return;
    }
    errors.fetch_add(1, std::memory_order_relaxed);
    if (!failing) {
      failing = true;
      if (quill::Logger* logger = log::engine(); logger != nullptr) {
        LOG_WARNING(logger, "sys.alert publish failed; the engine log is the only outage signal");
      }
    }
  }

  void run() {
    std::unique_lock lock(mu);
    for (;;) {
      const bool woken = cv.wait_for(lock, cfg.repeat, [this] { return stopping || change_count > 0; });
      if (stopping && change_count == 0) {
        return;
      }
      std::array<Change, kMaxChanges> batch = changes;
      std::size_t n = change_count;
      change_count = 0;
      if (!woken && active_since.has_value()) {
        batch.at(0) = Change{.active = true, .since_ms = *active_since};
        n = 1;
      }
      lock.unlock();
      for (std::size_t i = 0; i < n; ++i) {
        publish(batch.at(i));
      }
      lock.lock();
    }
  }

  void close() noexcept {
    if (nc != nullptr) {
      (void)natsConnection_FlushTimeout(nc, kStopFlushMs);
      natsConnection_Destroy(nc);
      nc = nullptr;
    }
    if (opts != nullptr) {
      natsOptions_Destroy(opts);
      opts = nullptr;
    }
  }
};

SystemAlertPublisher::SystemAlertPublisher(SystemAlertConfig cfg, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), clock)) {}

SystemAlertPublisher::~SystemAlertPublisher() { stop(); }

std::expected<void, Error> SystemAlertPublisher::start() {
  if (impl_->thread.joinable() || impl_->stopping) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument, "system alerts: already started"});
  }
  const bool configured =
      natsOptions_Create(&impl_->opts) == NATS_OK &&
      natsOptions_SetURL(impl_->opts, impl_->cfg.url.c_str()) == NATS_OK &&
      natsOptions_SetMaxReconnect(impl_->opts, -1) == NATS_OK &&
      natsOptions_SetReconnectWait(impl_->opts, kReconnectWaitMs) == NATS_OK &&
      natsOptions_SetRetryOnFailedConnect(impl_->opts, true, on_connected, nullptr) == NATS_OK;
  const natsStatus status = configured ? natsConnection_Connect(&impl_->nc, impl_->opts) : NATS_ERR;
  if (status != NATS_OK && status != NATS_NOT_YET_CONNECTED) {
    impl_->close();
    return std::unexpected(
        Error{ErrorCode::kConnectFailed, "system alerts: cannot set up the NATS connection"});
  }
  impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
  return {};
}

void SystemAlertPublisher::stop() {
  if (impl_->thread.joinable()) {
    {
      const std::scoped_lock lock(impl_->mu);
      impl_->stopping = true;
    }
    impl_->cv.notify_all();
    impl_->thread.join();
  }
  impl_->close();
}

// NOLINTNEXTLINE(bugprone-exception-escape): locking a healthy mutex; a failure there is fatal anyway
void SystemAlertPublisher::outage_started() noexcept {
  const std::scoped_lock lock(impl_->mu);
  if (impl_->active_since.has_value()) {
    return;
  }
  const std::int64_t since = impl_->clock->now() / kNanosPerMilli;
  impl_->active_since = since;
  impl_->add_change(Change{.active = true, .since_ms = since});
  impl_->cv.notify_all();
}

// NOLINTNEXTLINE(bugprone-exception-escape): locking a healthy mutex; a failure there is fatal anyway
void SystemAlertPublisher::outage_ended() noexcept {
  const std::scoped_lock lock(impl_->mu);
  const std::optional<std::int64_t> since = impl_->active_since;
  if (!since.has_value()) {
    return;
  }
  impl_->add_change(Change{.active = false, .since_ms = *since});
  impl_->active_since.reset();
  impl_->cv.notify_all();
}

std::uint64_t SystemAlertPublisher::published() const noexcept {
  return impl_->published.load(std::memory_order_relaxed);
}

std::uint64_t SystemAlertPublisher::publish_errors() const noexcept {
  return impl_->errors.load(std::memory_order_relaxed);
}

}  // namespace vyuha::bus

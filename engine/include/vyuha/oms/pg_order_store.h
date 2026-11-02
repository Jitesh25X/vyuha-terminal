#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "vyuha/core/error.h"
#include "vyuha/core/spsc_ring.h"
#include "vyuha/oms/oms.h"

namespace vyuha::oms {

// The Postgres event writer thread (ARCHITECTURE §4). Each batch updates `order` and `order_event` in one
// transaction, so they can't disagree. A failed batch is retried, in order, until it commits: events are
// dropped while running. While a batch fails, healthy() is false, so the OMS sends nothing to a broker, and
// when the ring fills it refuses every transition (fail closed). Shutdown retries for a bounded time, then
// logs what was lost; recovery reconciles it with the broker.
class PgOrderEventWriter final : public IOrderEventSink {
 public:
  explicit PgOrderEventWriter(std::string url);
  PgOrderEventWriter(const PgOrderEventWriter&) = delete;
  PgOrderEventWriter& operator=(const PgOrderEventWriter&) = delete;
  PgOrderEventWriter(PgOrderEventWriter&&) = delete;
  PgOrderEventWriter& operator=(PgOrderEventWriter&&) = delete;
  ~PgOrderEventWriter() override;  // writes what's queued; gives up after kShutdownRetry if it can't

  // Core thread.
  [[nodiscard]] bool try_push(const OrderEventRecord& event) noexcept override;

  [[nodiscard]] std::uint64_t pushed() const noexcept override { return pushed_; }  // core thread
  [[nodiscard]] std::uint64_t durable() const noexcept override { return written(); }
  [[nodiscard]] std::uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
  // False from a failed batch until the next one commits.
  [[nodiscard]] bool healthy() const noexcept override { return healthy_.load(std::memory_order_acquire); }

 private:
  static constexpr std::size_t kRingSize = 16'384;
  static constexpr std::size_t kBatchSize = 256;
  static constexpr std::chrono::seconds kShutdownRetry{5};

  void run(const std::stop_token& stop);
  [[nodiscard]] bool flush(const std::vector<OrderEventRecord>& batch);

  std::string url_;
  std::unique_ptr<SpscRing<OrderEventRecord, kRingSize>> ring_;
  std::uint64_t pushed_{0};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<bool> healthy_{true};
  std::jthread thread_;
};

// Every non-terminal order, for Oms::restore on engine start. Fails rather than guess if an order's status
// disagrees with its last event. Details of a database error are never surfaced.
[[nodiscard]] std::expected<std::vector<RecoveredOrder>, Error> load_open_orders(const std::string& url);

}  // namespace vyuha::oms

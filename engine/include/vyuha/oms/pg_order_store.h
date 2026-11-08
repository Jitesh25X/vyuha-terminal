#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "vyuha/core/error.h"
#include "vyuha/core/outage_signal.h"
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
  // `outage_signal` carries the outage to the desk without Postgres, and must outlive the writer. Required,
  // so the wiring can't be forgotten; only a test passes nullptr.
  PgOrderEventWriter(std::string url, IOutageSignal* outage_signal);
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
  [[nodiscard]] bool try_pop_refused(Uuid& order_id) noexcept override { return refused_->try_pop(order_id); }
  // False from a failed batch until the next one commits.
  [[nodiscard]] bool healthy() const noexcept override { return healthy_.load(std::memory_order_acquire); }
  // Counted; the first batch that commits after an outage is followed by one ORDER_LOG_OUTAGE alert with
  // the outage's length and these counts (ADR-027).
  void note_outage_refusal(OutageRefusal kind) noexcept override;

 private:
  static constexpr std::size_t kRingSize = 16'384;
  static constexpr std::size_t kBatchSize = 256;
  static constexpr std::size_t kRefusedRingSize = 1'024;
  static constexpr std::chrono::seconds kShutdownRetry{5};

  void run(const std::stop_token& stop);
  [[nodiscard]] bool flush(const std::vector<OrderEventRecord>& batch, const std::stop_token& stop);
  // After a data error: one event at a time, so the bad one fails alone. False on anything that isn't a data
  // error.
  [[nodiscard]] bool flush_each(const std::vector<OrderEventRecord>& batch, const std::stop_token& stop);
  void quarantine(const OrderEventRecord& event, std::string_view sqlstate, const std::stop_token& stop);
  static constexpr std::chrono::seconds kOutageReportRetry{1};
  // Writer thread. After the first commit following an outage, and again while idle until it lands (one try
  // per kOutageReportRetry): one ORDER_LOG_OUTAGE alert with the outage's length and the refusal counts.
  void report_outage_if_due() noexcept;
  void signal_outage_started() noexcept;
  void signal_outage_ended() noexcept;
  [[nodiscard]] bool report_outage() noexcept;
  // At stop: an outage report that never landed is at least in the log.
  void log_unreported_outage() noexcept;

  std::string url_;
  IOutageSignal* outage_signal_{nullptr};
  bool outage_signalled_{false};  // writer thread only
  std::unique_ptr<SpscRing<OrderEventRecord, kRingSize>> ring_;
  std::unique_ptr<SpscRing<Uuid, kRefusedRingSize>> refused_;
  std::unordered_set<std::string> quarantined_;  // writer thread only: their later events are skipped
  std::uint64_t pushed_{0};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<bool> healthy_{true};
  std::array<std::atomic<std::uint64_t>, kOutageRefusalKinds> outage_refusals_{};
  std::optional<std::chrono::steady_clock::time_point> outage_since_;  // writer thread only
  std::chrono::steady_clock::time_point next_report_try_;              // writer thread only
  std::jthread thread_;
};

// Every non-terminal order, for Oms::restore on engine start. Fails rather than guess if an order's status
// disagrees with its last event. Details of a database error are never surfaced.
[[nodiscard]] std::expected<std::vector<RecoveredOrder>, Error> load_open_orders(const std::string& url);

}  // namespace vyuha::oms

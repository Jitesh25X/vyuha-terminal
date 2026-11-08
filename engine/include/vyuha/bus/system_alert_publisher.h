#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "vyuha/core/clock.h"
#include "vyuha/core/error.h"
#include "vyuha/core/outage_signal.h"

namespace vyuha::bus {

struct SystemAlertConfig {
  std::string url{"nats://127.0.0.1:4222"};
  std::string subject{"sys.alert"};
  // While an outage holds it is re-published this often, so a gateway that restarts or missed the first one
  // still shows it, and a gateway that stops hearing it (engine gone) lets the banner lapse.
  std::chrono::milliseconds repeat{5000};
};

// Publishes the order-log outage on NATS `sys.alert` (common.v1.SystemAlert, ADR-027), from its own thread on
// its own connection: the toast and the banner don't depend on Postgres. Core NATS, so at most once. If NATS
// is down too, the engine log and the gateway's engine-health indicator are what's left (owner, D-19).
class SystemAlertPublisher final : public IOutageSignal {
 public:
  SystemAlertPublisher(SystemAlertConfig cfg, const IClock& clock);
  SystemAlertPublisher(SystemAlertConfig cfg, const IClock&& clock) = delete;
  SystemAlertPublisher(const SystemAlertPublisher&) = delete;
  SystemAlertPublisher& operator=(const SystemAlertPublisher&) = delete;
  SystemAlertPublisher(SystemAlertPublisher&&) = delete;
  SystemAlertPublisher& operator=(SystemAlertPublisher&&) = delete;
  ~SystemAlertPublisher() override;

  // Doesn't need NATS to be up: the connection keeps trying in the background. An error is a bad URL or a
  // second start.
  [[nodiscard]] std::expected<void, Error> start();
  void stop();

  void outage_started() noexcept override;
  void outage_ended() noexcept override;

  [[nodiscard]] std::uint64_t published() const noexcept;
  [[nodiscard]] std::uint64_t publish_errors() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vyuha::bus

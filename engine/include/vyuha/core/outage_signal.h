#pragma once

namespace vyuha {

// Told when the order log stops and starts being durable (ADR-027), by something that doesn't need Postgres.
// Called from the writer thread: an implementation must not block it.
class IOutageSignal {
 public:
  IOutageSignal() = default;
  IOutageSignal(const IOutageSignal&) = delete;
  IOutageSignal& operator=(const IOutageSignal&) = delete;
  IOutageSignal(IOutageSignal&&) = delete;
  IOutageSignal& operator=(IOutageSignal&&) = delete;
  virtual ~IOutageSignal() = default;

  virtual void outage_started() noexcept = 0;
  virtual void outage_ended() noexcept = 0;
};

}  // namespace vyuha

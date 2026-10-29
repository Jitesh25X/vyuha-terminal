#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "vyuha/core/error.h"
#include "vyuha/session/instrument_directory.h"

namespace vyuha::session {

// Instrument master from Postgres (active rows of `instrument`, maintained by the quant sync,
// F-MD-1): canonical id → Kite token / Upstox key. Loaded at startup; refresh() after the
// 08:00 IST sync. Lookups take a shared lock; a refresh swaps the whole map at once.
class PgInstrumentDirectory final : public InstrumentDirectory {
 public:
  explicit PgInstrumentDirectory(std::string postgres_url) : url_(std::move(postgres_url)) {}

  // Reloads all active instruments; returns how many have at least one broker id.
  [[nodiscard]] std::expected<std::size_t, Error> refresh();
  [[nodiscard]] std::optional<BrokerIds> find(md::InstrumentId id) const override;
  [[nodiscard]] std::size_t size() const;

 private:
  std::string url_;  // credentials: never logged
  mutable std::shared_mutex mu_;
  std::shared_ptr<const std::unordered_map<md::InstrumentId, BrokerIds>> map_ =
      std::make_shared<const std::unordered_map<md::InstrumentId, BrokerIds>>();
};

}  // namespace vyuha::session

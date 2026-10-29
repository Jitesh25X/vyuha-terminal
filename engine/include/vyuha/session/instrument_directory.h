#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vyuha/md/tick.h"

namespace vyuha::session {

// Broker identifiers of one canonical instrument (instrument.kite_token / instrument.upstox_key).
struct BrokerIds {
  std::optional<std::uint32_t> kite_token;
  std::string upstox_key;  // empty = not listed on Upstox
};

// Resolves canonical instrument ids for subscriptions. M1.7 adds a Postgres-backed implementation
// (libpqxx) over the `instrument` master; tests and tools use the static one.
class InstrumentDirectory {
 public:
  InstrumentDirectory() = default;
  InstrumentDirectory(const InstrumentDirectory&) = delete;
  InstrumentDirectory& operator=(const InstrumentDirectory&) = delete;
  InstrumentDirectory(InstrumentDirectory&&) = delete;
  InstrumentDirectory& operator=(InstrumentDirectory&&) = delete;
  virtual ~InstrumentDirectory() = default;
  [[nodiscard]] virtual std::optional<BrokerIds> find(md::InstrumentId id) const = 0;
};

class StaticInstrumentDirectory final : public InstrumentDirectory {
 public:
  explicit StaticInstrumentDirectory(std::vector<std::pair<md::InstrumentId, BrokerIds>> entries)
      : map_(entries.begin(), entries.end()) {}
  [[nodiscard]] std::optional<BrokerIds> find(md::InstrumentId id) const override {
    const auto it = map_.find(id);
    return it == map_.end() ? std::nullopt : std::optional<BrokerIds>(it->second);
  }

 private:
  std::unordered_map<md::InstrumentId, BrokerIds> map_;
};

}  // namespace vyuha::session

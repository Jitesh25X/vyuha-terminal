#include "vyuha/session/pg_instrument_directory.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <pqxx/pqxx>

namespace vyuha::session {

std::expected<std::size_t, Error> PgInstrumentDirectory::refresh() {
  auto next = std::make_shared<std::unordered_map<md::InstrumentId, BrokerIds>>();
  try {
    pqxx::connection conn(url_);
    pqxx::read_transaction tx(conn);
    for (const auto& [id, kite, upstox] :
         tx.query<std::int64_t, std::optional<std::int64_t>, std::optional<std::string>>(
             "SELECT id, kite_token, upstox_key FROM instrument "
             "WHERE valid_to IS NULL AND (kite_token IS NOT NULL OR upstox_key IS NOT NULL)")) {
      BrokerIds ids;
      if (kite.has_value() && *kite > 0 && *kite <= UINT32_MAX) {
        ids.kite_token = static_cast<std::uint32_t>(*kite);
      }
      ids.upstox_key = upstox.value_or(std::string{});
      next->emplace(static_cast<md::InstrumentId>(id), std::move(ids));
    }
  } catch (const pqxx::failure&) {
    // Message may echo connection details: never surfaced.
    return std::unexpected(Error{ErrorCode::kConnectFailed, "instruments: cannot load from Postgres"});
  }
  const std::size_t n = next->size();
  {
    const std::unique_lock lock(mu_);
    map_ = std::move(next);
  }
  return n;
}

std::optional<BrokerIds> PgInstrumentDirectory::find(md::InstrumentId id) const {
  std::shared_ptr<const std::unordered_map<md::InstrumentId, BrokerIds>> map;
  {
    const std::shared_lock lock(mu_);
    map = map_;
  }
  const auto it = map->find(id);
  return it == map->end() ? std::nullopt : std::optional<BrokerIds>(it->second);
}

std::size_t PgInstrumentDirectory::size() const {
  const std::shared_lock lock(mu_);
  return map_->size();
}

}  // namespace vyuha::session

#include "vyuha/risk/margin_cache.h"

namespace vyuha::risk {

namespace {

[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t x) noexcept {
  x ^= x >> 30U;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27U;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31U;
  return x;
}

}  // namespace

MarginCache::MarginCache(std::size_t max_accounts, std::size_t max_instruments)
    : accounts_(max_accounts), lots_(max_instruments), lot_index_(max_instruments) {}

bool MarginCache::set_available(const oms::Uuid& account, std::int64_t amount, Nanos as_of) noexcept {
  for (std::size_t i = 0; i < account_count_; ++i) {
    if (accounts_[i].account == account) {
      accounts_[i].amount = amount;
      accounts_[i].reserved = 0;
      accounts_[i].as_of = as_of;
      return true;
    }
  }
  if (account_count_ == accounts_.size()) {
    return false;
  }
  accounts_[account_count_++] =
      Available{.account = account, .amount = amount, .reserved = 0, .as_of = as_of};
  return true;
}

bool MarginCache::set_per_lot(std::int64_t instrument_id, std::int64_t amount, Nanos as_of) noexcept {
  if (amount <= 0) {
    return false;  // a margin of nothing would let every order through
  }
  const std::uint64_t hash = mix(static_cast<std::uint64_t>(instrument_id));
  const std::int32_t found = lot_index_.find(hash, [&](std::int32_t s) noexcept {
    return lots_[static_cast<std::size_t>(s)].instrument_id == instrument_id;
  });
  if (found >= 0) {
    lots_[static_cast<std::size_t>(found)].amount = amount;
    lots_[static_cast<std::size_t>(found)].as_of = as_of;
    return true;
  }
  if (lot_count_ == lots_.size()) {
    return false;
  }
  const auto slot = static_cast<std::int32_t>(lot_count_++);
  lots_[static_cast<std::size_t>(slot)] =
      PerLot{.instrument_id = instrument_id, .amount = amount, .as_of = as_of};
  lot_index_.insert(hash, slot);
  return true;
}

bool MarginCache::reserve(const oms::Uuid& account, Wide amount) noexcept {
  for (std::size_t i = 0; i < account_count_; ++i) {
    if (accounts_[i].account == account) {
      accounts_[i].reserved += amount;
      return true;
    }
  }
  return false;
}

const MarginCache::Available* MarginCache::available(const oms::Uuid& account) const noexcept {
  for (std::size_t i = 0; i < account_count_; ++i) {
    if (accounts_[i].account == account) {
      return &accounts_[i];
    }
  }
  return nullptr;
}

const MarginCache::PerLot* MarginCache::per_lot(std::int64_t instrument_id) const noexcept {
  const std::int32_t found =
      lot_index_.find(mix(static_cast<std::uint64_t>(instrument_id)), [&](std::int32_t s) noexcept {
        return lots_[static_cast<std::size_t>(s)].instrument_id == instrument_id;
      });
  return found < 0 ? nullptr : &lots_[static_cast<std::size_t>(found)];
}

}  // namespace vyuha::risk

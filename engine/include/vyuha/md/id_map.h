#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "vyuha/md/tick.h"

namespace vyuha::md {

// Fixed-capacity open-addressing map InstrumentId → V. Storage is allocated once in the
// constructor; insert and lookup never allocate (core-thread hot path). Linear probing, no erase
// (the books are rebuilt per trading day). Key 0 marks an empty slot, so id 0 is not storable.
template <class V>
class IdMap {
 public:
  // Holds at least `max_entries` keys at a load factor ≤ 0.5.
  explicit IdMap(std::size_t max_entries)
      : cap_(std::bit_ceil(std::max<std::size_t>(max_entries * 2, 16))),
        keys_(cap_, 0),
        values_(cap_),
        limit_(max_entries) {
    // Checked here, not as a class constraint: nested value types aren't complete at that point.
    static_assert(std::is_nothrow_default_constructible_v<V>);
  }

  [[nodiscard]] V* find(InstrumentId id) noexcept {
    const std::size_t i = probe(id);
    return keys_[i] == id && id != 0 ? &values_[i] : nullptr;
  }
  [[nodiscard]] const V* find(InstrumentId id) const noexcept {
    const std::size_t i = probe(id);
    return keys_[i] == id && id != 0 ? &values_[i] : nullptr;
  }

  // Existing or newly default-constructed value; nullptr when id is 0 or the map is full.
  [[nodiscard]] V* find_or_insert(InstrumentId id) noexcept {
    if (id == 0) {
      return nullptr;
    }
    const std::size_t i = probe(id);
    if (keys_[i] != id) {
      if (size_ == limit_) {
        return nullptr;
      }
      keys_[i] = id;
      ++size_;
    }
    return &values_[i];
  }

  template <class F>
  void for_each(F f) noexcept {
    for (std::size_t i = 0; i < cap_; ++i) {
      if (keys_[i] != 0) {
        f(keys_[i], values_[i]);
      }
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t max_entries() const noexcept { return limit_; }

 private:
  // Slot holding `id`, or the empty slot where it would go (load ≤ 0.5 guarantees one exists).
  [[nodiscard]] std::size_t probe(InstrumentId id) const noexcept {
    // Fibonacci hashing spreads sequential ids (bigserial) across the table.
    std::size_t i = static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> 32U) & (cap_ - 1);
    while (keys_[i] != id && keys_[i] != 0) {
      i = (i + 1) & (cap_ - 1);
    }
    return i;
  }

  std::size_t cap_;
  std::vector<InstrumentId> keys_;
  std::vector<V> values_;
  std::size_t limit_;
  std::size_t size_{0};
};

}  // namespace vyuha::md

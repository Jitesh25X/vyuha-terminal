#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vyuha::risk::detail {

// A hash index over a pool kept elsewhere: it maps a hash to the pool slot whose key matches. It never
// removes, and it holds at most half full, so a probe always ends.
class SlotIndex {
 public:
  explicit SlotIndex(std::size_t entries) {
    std::size_t n = 16;
    while (n < entries * 2) {
      n *= 2;
    }
    slots_.assign(n, -1);
  }
  template <class Match>
  [[nodiscard]] std::int32_t find(std::uint64_t hash, Match matches) const noexcept {
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
      const std::int32_t slot = slots_[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      if (slot < 0 || matches(slot)) {
        return slot;
      }
    }
  }
  void insert(std::uint64_t hash, std::int32_t slot) noexcept {
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = hash & mask;
    while (slots_[i] >= 0) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      i = (i + 1) & mask;
    }
    slots_[i] = slot;  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  }

 private:
  std::vector<std::int32_t> slots_;
};

}  // namespace vyuha::risk::detail

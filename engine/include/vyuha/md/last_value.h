#pragma once

#include <cstddef>

#include "vyuha/md/id_map.h"
#include "vyuha/md/tick.h"

namespace vyuha::md {

// Latest tick per instrument (core thread only). Fixed capacity, no allocation per update.
class LastValueBook {
 public:
  explicit LastValueBook(std::size_t max_instruments) : map_(max_instruments) {}

  // False when the book is full (the tick is not stored; callers count it).
  [[nodiscard]] bool update(const Tick& tick) noexcept;
  [[nodiscard]] const Tick* find(InstrumentId id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

 private:
  struct Entry {
    Tick tick;
    bool set{false};
  };
  IdMap<Entry> map_;
};

}  // namespace vyuha::md

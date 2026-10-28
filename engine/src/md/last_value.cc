#include "vyuha/md/last_value.h"

namespace vyuha::md {

bool LastValueBook::update(const Tick& tick) noexcept {
  Entry* e = map_.find_or_insert(tick.instrument_id);
  if (e == nullptr) {
    return false;
  }
  e->tick = tick;
  e->set = true;
  return true;
}

const Tick* LastValueBook::find(InstrumentId id) const noexcept {
  const Entry* e = map_.find(id);
  return e != nullptr && e->set ? &e->tick : nullptr;
}

}  // namespace vyuha::md
